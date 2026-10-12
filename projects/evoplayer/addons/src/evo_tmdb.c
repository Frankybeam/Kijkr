/*
 * Module: evo_tmdb - see addons/include/evo_tmdb.h.
 */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "evo_tmdb.h"
#include "evo_net.h"
#include "cJSON.h"
#include "evo_data_path.h"
#include "evo_provider_log.h"
#include "evo_title_tag.h"
#include "provider_stalker.h"
#include "evo_i18n.h"

#define TMDB_CONF   "tmdb.conf"
#define TMDB_MATCH  "tmdb_match2.txt"   /* per source; the old file had one portal */
#define TMDB_API    "https://api.themoviedb.org/3"
#define TMDB_IMG    "https://image.tmdb.org/t/p/w342"

#define LIST_TTL     (6 * 3600)      /* the lists, refreshed every six hours */
#define LIST_RETRY   (10 * 60)       /* after a failure */
#define FOUND_TTL    (7 * 86400)     /* "it is in the portal" holds a week */
#define MISSING_TTL  (86400)         /* "it is not" is asked again after a day */
#define MATCH_SLOTS  900

/* ------------------------------------------------------------------------- */
/* State                                                                     */
/* ------------------------------------------------------------------------- */

static char g_key[EVO_TMDB_KEY_MAX];
static int  g_key_loaded;

static evo_tmdb_item_t g_list[EVO_TMDB_LISTS][EVO_TMDB_PER_LIST];
static int             g_list_n[EVO_TMDB_LISTS];
static time_t          g_list_at[EVO_TMDB_LISTS];   /* last success */
static time_t          g_list_try[EVO_TMDB_LISTS];  /* last attempt */
static int             g_list_busy[EVO_TMDB_LISTS];
static unsigned        g_gen = 1;                    /* bumped by a reset */

/* Where titles are looked up: "stalker#0", "stalker#1", "xtream", "iptv". */
#define MAX_TARGETS 12
static char g_tgt[MAX_TARGETS][EVO_PROVIDER_MAX_ID + 8];
static int  g_tgt_n;
static char g_tgt_sig[MAX_TARGETS * (EVO_PROVIDER_MAX_ID + 9)];
static int  g_find_busy;
static time_t g_find_pause_until;
static char g_error[160];

static int    g_tgt_fail[MAX_TARGETS];        /* answers missed in a row */
static time_t g_tgt_down_until[MAX_TARGETS];  /* a source that keeps failing is left alone a while */

/* One source not answering must not hold up the others: a short pause, and
 * after three misses in a row it is skipped for ten minutes. */
static void target_failed(int t)
{
    if (t < 0 || t >= MAX_TARGETS) return;
    const time_t now = time(NULL);
    if (++g_tgt_fail[t] >= 3) {
        g_tgt_down_until[t] = now + 600;
        g_tgt_fail[t] = 0;
        PROV_LOG("tmdb: %s does not answer - skipped for ten minutes", g_tgt[t]);
    } else {
        g_find_pause_until = now + 10;
    }
}

typedef struct {
    char   prov[EVO_PROVIDER_MAX_ID];
    char   tid[16];                                  /* "v12345" / "s678" */
    long   when;
    int    found;
    int    folder;
    char   key[EVO_PROVIDER_MAX_ITEM_ID + 32];
    char   versions[EVO_TMDB_VERSIONS_SZ];
} match_t;
static match_t g_match[MATCH_SLOTS];
static int     g_match_n;
static int     g_match_loaded;
static int     g_match_dirty;       /* changed since the file was written */
static time_t  g_match_saved;

/* ------------------------------------------------------------------------- */
/* The key                                                                   */
/* ------------------------------------------------------------------------- */

static void key_load(void)
{
    if (g_key_loaded) return;
    g_key_loaded = 1;
    g_key[0] = '\0';
    FILE *f = fopen(evo_data_path(TMDB_CONF), "r");
    if (!f) return;
    if (fgets(g_key, sizeof g_key, f)) {
        size_t n = strlen(g_key);
        while (n && (g_key[n - 1] == '\n' || g_key[n - 1] == '\r' || g_key[n - 1] == ' ')) g_key[--n] = '\0';
    }
    fclose(f);
}

const char *evo_tmdb_key(void)
{
    key_load();
    return g_key;
}

int evo_tmdb_set_key(const char *key)
{
    char k[EVO_TMDB_KEY_MAX];
    snprintf(k, sizeof k, "%s", key ? key : "");
    /* Trim what a phone keyboard likes to add. */
    char *b = k;
    while (*b == ' ' || *b == '\t') b++;
    size_t n = strlen(b);
    while (n && (b[n - 1] == ' ' || b[n - 1] == '\t' || b[n - 1] == '\r' || b[n - 1] == '\n')) b[--n] = '\0';
    if (n >= EVO_TMDB_KEY_MAX - 1) return 0;
    for (size_t i = 0; i < n; ++i)
        if (isspace((unsigned char)b[i]) || (unsigned char)b[i] < 0x21 || (unsigned char)b[i] > 0x7e) return 0;

    key_load();
    snprintf(g_key, sizeof g_key, "%s", b);
    if (!g_key[0]) {
        remove(evo_data_path(TMDB_CONF));
    } else {
        FILE *f = fopen(evo_data_path(TMDB_CONF), "w");
        if (!f) return 0;
        fprintf(f, "%s\n", g_key);
        fclose(f);
    }
    evo_tmdb_reset();
    return 1;
}

/* A v3 key is 32 hex characters and goes in the query; anything else is a
 * v4 read token and goes in an Authorization header. */
static int key_is_v3(void)
{
    if (strlen(g_key) != 32) return 0;
    for (const char *c = g_key; *c; ++c)
        if (!isxdigit((unsigned char)*c)) return 0;
    return 1;
}

/* ------------------------------------------------------------------------- */
/* Matching a portal title to a TMDB title                                   */
/* ------------------------------------------------------------------------- */

/* The base letter of a two-byte UTF-8 Latin-1 letter (0xC3 0x80..0xBF), or 0. */
static char latin1_base(unsigned char b)
{
    static const char map[64] =
        "AAAAAAACEEEEIIII" "DNOOOOOxOUUUUYTs"
        "aaaaaaaceeeeiiii" "dnooooo/ouuuuyty";
    return (b >= 0x80 && b <= 0xBF) ? map[b - 0x80] : 0;
}

/*
 * The comparable form of a title: labels gone (MULTI, NL, 4K), accents folded,
 * lower case, punctuation as spaces, "&" as "and", no year at the end, no
 * leading "the". "MULTI Amélie (2001)" and "Amelie" both become "amelie".
 */
/* A quality / language label at the end: "Dune Part Two 4K", "Dune (NL)". */
static int tail_label(const char *s, size_t n)
{
    char up[16];
    if (n == 0 || n >= sizeof up) return 0;
    for (size_t i = 0; i < n; ++i) up[i] = (char)toupper((unsigned char)s[i]);
    up[n] = '\0';
    if (evo_title_tag_is_label(up, n)) return 1;
    static const char *const extra[] = { "HD", "SD", "HDR", "HDR10", "DV", "1080P", "720P", "2160P",
                                         "HEVC", "X264", "X265", "H264", "H265", "REMUX", "WEB",
                                         "WEBDL", "BLURAY", "DOLBY", "ATMOS", "3D", NULL };
    for (int i = 0; extra[i]; ++i)
        if (strcmp(up, extra[i]) == 0) return 1;
    return 0;
}

static void strip_tail_labels(char *t)
{
    for (int round = 0; round < 6; ++round) {
        size_t n = strlen(t);
        while (n && evo_title_tag_wrap(t[n - 1])) n--;            /* " - ", "]", "|" */
        while (n && (t[n - 1] == ')' || t[n - 1] == ' ')) n--;
        size_t b = n;
        while (b && isalnum((unsigned char)t[b - 1])) b--;
        if (b == n || !tail_label(t + b, n - b)) return;
        if (b == 0) return;                                         /* keep the title itself */
        size_t cut = b;
        while (cut && (evo_title_tag_wrap(t[cut - 1]) || t[cut - 1] == '(')) cut--;
        if (cut == 0) return;
        t[cut] = '\0';
    }
}

static void tm_norm(const char *in, char *out, size_t sz)
{
    char tmp[256], w[256];
    evo_title_strip_tags(in ? in : "", tmp, sizeof tmp);
    strip_tail_labels(tmp);
    size_t k = 0;
    for (const unsigned char *c = (const unsigned char *)tmp; *c && k + 5 < sizeof w; ++c) {
        if (*c == 0xC3 && c[1]) {
            char b = latin1_base(c[1]);
            ++c;
            if (b && isalpha((unsigned char)b)) { w[k++] = (char)tolower((unsigned char)b); continue; }
            w[k++] = ' ';
            continue;
        }
        if (*c == '&') { memcpy(w + k, " and ", 5); k += 5; continue; }
        if (*c == '\'' ) continue;                     /* "Schindler's" = "Schindlers" */
        if (isalnum(*c)) { w[k++] = (char)tolower(*c); continue; }
        if (*c >= 0x80) { w[k++] = (char)*c; continue; } /* other scripts stay as they are */
        w[k++] = ' ';
    }
    w[k] = '\0';

    /* Collapse spaces into the output. */
    size_t o = 0;
    int space = 1;
    for (size_t i = 0; w[i] && o + 1 < sz; ++i) {
        if (w[i] == ' ') { if (!space) { out[o++] = ' '; space = 1; } continue; }
        out[o++] = w[i];
        space = 0;
    }
    while (o && out[o - 1] == ' ') o--;
    out[o] = '\0';

    /* A year at the end: "dune part two 2024" -> "dune part two". */
    if (o > 5 && out[o - 5] == ' ' && isdigit((unsigned char)out[o - 4]) &&
        isdigit((unsigned char)out[o - 3]) && isdigit((unsigned char)out[o - 2]) &&
        isdigit((unsigned char)out[o - 1]) && (out[o - 4] == '1' || out[o - 4] == '2')) {
        o -= 5;
        out[o] = '\0';
    }
    if (strncmp(out, "the ", 4) == 0) memmove(out, out + 4, strlen(out + 4) + 1);
}

/* The year a portal row knows: its subtitle ("2024", "2024-03-01") or a
 * "(2024)" in its name. 0 when neither. */
static int tm_year(const char *title, const char *year)
{
    if (year && isdigit((unsigned char)year[0]) && isdigit((unsigned char)year[3])) {
        const int y = atoi(year);
        if (y > 1900 && y < 2100) return y;
    }
    for (const char *p = title ? strchr(title, '(') : NULL; p; p = strchr(p + 1, '(')) {
        if (isdigit((unsigned char)p[1]) && isdigit((unsigned char)p[4]) && p[5] == ')') {
            const int y = atoi(p + 1);
            if (y > 1900 && y < 2100) return y;
        }
    }
    return 0;
}

int evo_tmdb_same_title(const char *portal_title, const char *portal_year,
                        const char *tmdb_title, const char *tmdb_year, int is_film)
{
    char a[256], b[256];
    tm_norm(portal_title, a, sizeof a);
    tm_norm(tmdb_title, b, sizeof b);
    if (!a[0] || strcmp(a, b) != 0) return 0;
    if (!is_film) return 1;                 /* a series' year is its first season, the portal's often not */
    const int ya = tm_year(portal_title, portal_year), yb = tm_year("", tmdb_year);
    return !ya || !yb || abs(ya - yb) <= 1;
}

/* ------------------------------------------------------------------------- */
/* What the portal was found to have, kept between launches                  */
/* ------------------------------------------------------------------------- */

static void match_load(void)
{
    if (g_match_loaded) return;
    g_match_loaded = 1;
    g_match_n = 0;
    FILE *f = fopen(evo_data_path(TMDB_MATCH), "r");
    if (!f) return;
    char line[EVO_TMDB_VERSIONS_SZ + 512];
    while (g_match_n < MATCH_SLOTS && fgets(line, sizeof line, f)) {
        match_t *m = &g_match[g_match_n];
        memset(m, 0, sizeof *m);
        char *prov = strtok(line, "\t");
        char *tid = strtok(NULL, "\t");
        char *when = strtok(NULL, "\t");
        char *found = strtok(NULL, "\t");
        char *folder = strtok(NULL, "\t");
        char *key = strtok(NULL, "\t\r\n");
        char *vers = strtok(NULL, "\t\r\n");          /* absent in older files */
        if (!prov || !tid || !when || !found || !folder) continue;
        if (vers && strcmp(vers, "-") != 0) snprintf(m->versions, sizeof m->versions, "%s", vers);
        snprintf(m->prov, sizeof m->prov, "%s", prov);
        snprintf(m->tid, sizeof m->tid, "%s", tid);
        m->when = atol(when);
        m->found = atoi(found);
        m->folder = atoi(folder);
        snprintf(m->key, sizeof m->key, "%s", (key && strcmp(key, "-") != 0) ? key : "");
        g_match_n++;
    }
    fclose(f);
}

static void match_write(FILE *f)
{
    for (int i = 0; i < g_match_n; ++i) {
        const match_t *m = &g_match[i];
        fprintf(f, "%s\t%s\t%ld\t%d\t%d\t%s\t%s\n", m->prov, m->tid, m->when, m->found, m->folder,
                m->key[0] ? m->key : "-", m->versions[0] ? m->versions : "-");
    }
}

static void match_save(void)
{
    /* Written next to it and then renamed, so a stop half-way never leaves
     * half a file; where rename is refused, written in place as before. */
    char tmp[600], dst[600];
    snprintf(dst, sizeof dst, "%s", evo_data_path(TMDB_MATCH));
    snprintf(tmp, sizeof tmp, "%s.tmp", dst);
    FILE *f = fopen(tmp, "w");
    if (f) {
        match_write(f);
        fclose(f);
        if (rename(tmp, dst) == 0) return;
        remove(dst);
        if (rename(tmp, dst) == 0) return;
        remove(tmp);
    }
    f = fopen(dst, "w");
    if (!f) return;
    match_write(f);
    fclose(f);
}

static match_t *match_find(const char *prov, const char *tid)
{
    for (int i = 0; i < g_match_n; ++i)
        if (strcmp(g_match[i].prov, prov) == 0 && strcmp(g_match[i].tid, tid) == 0) return &g_match[i];
    return NULL;
}

static void match_put(const char *prov, const char *tid, int found, int folder, const char *key,
                      const char *versions)
{
    match_t *m = match_find(prov, tid);
    if (!m) {
        if (g_match_n >= MATCH_SLOTS) {               /* full: the oldest goes */
            int old = 0;
            for (int i = 1; i < g_match_n; ++i)
                if (g_match[i].when < g_match[old].when) old = i;
            m = &g_match[old];
        } else {
            m = &g_match[g_match_n++];
        }
    }
    memset(m, 0, sizeof *m);
    snprintf(m->prov, sizeof m->prov, "%s", prov);
    snprintf(m->tid, sizeof m->tid, "%s", tid);
    m->when = (long)time(NULL);
    m->found = found;
    m->folder = folder;
    snprintf(m->key, sizeof m->key, "%s", key ? key : "");
    snprintf(m->versions, sizeof m->versions, "%s", versions ? versions : "");
    g_match_dirty = 1;               /* written by evo_tmdb_tick, at most every few seconds */
}

static void item_tid(const evo_tmdb_item_t *it, char *out, size_t sz)
{
    snprintf(out, sz, "%c%d", it->type, it->id);
}

/* ------------------------------------------------------------------------- */
/* The lists                                                                 */
/* ------------------------------------------------------------------------- */

typedef struct {
    int      list;
    unsigned gen;
    int      local;                  /* the same list in the TMDB language: titles and stories */
} list_ctx_t;

/* ---- the language of titles and stories ---- */
#define TMDB_LANG_CONF "tmdb_lang.conf"
static const char *const k_tags[EVO_LANG_COUNT] = { "en-US", "nl-NL", "de-DE", "fr-FR", "es-ES" };
static int    g_lang_choice = -1;              /* -1 not read yet */
static char   g_tag[8] = "en-US";              /* what the lists were fetched in */
static int    g_local_due[EVO_TMDB_LISTS];
static int    g_local_busy[EVO_TMDB_LISTS];

int evo_tmdb_lang_choice(void)
{
    if (g_lang_choice < 0) {
        g_lang_choice = 0;
        FILE *f = fopen(evo_data_path(TMDB_LANG_CONF), "r");
        if (f) {
            char b[8] = "";
            if (fgets(b, sizeof b, f)) g_lang_choice = atoi(b);
            fclose(f);
        }
        if (g_lang_choice < 0 || g_lang_choice >= EVO_TMDB_LANG_CHOICES) g_lang_choice = 0;
    }
    return g_lang_choice;
}

void evo_tmdb_set_lang_choice(int choice)
{
    if (choice < 0 || choice >= EVO_TMDB_LANG_CHOICES) choice = 0;
    g_lang_choice = choice;
    FILE *f = fopen(evo_data_path(TMDB_LANG_CONF), "w");
    if (f) { fprintf(f, "%d\n", choice); fclose(f); }
}

/* The TMDB language now: the choice, or EVO's own language. */
static const char *lang_tag(void)
{
    const int c = evo_tmdb_lang_choice();
    const int l = c > 0 ? c - 1 : evo_lang_get();
    return (l >= 0 && l < EVO_LANG_COUNT) ? k_tags[l] : k_tags[0];
}

static void json_s(const cJSON *o, const char *k, char *out, size_t sz)
{
    out[0] = '\0';
    const cJSON *v = cJSON_GetObjectItem(o, k);
    if (v && cJSON_IsString(v) && v->valuestring) snprintf(out, sz, "%s", v->valuestring);
}

static void on_list(int success, int status, const char *body, size_t len, void *ud)
{
    (void)len;
    list_ctx_t *c = (list_ctx_t *)ud;
    const int list = c->list;
    const unsigned gen = c->gen;
    const int local = c->local;
    free(c);
    if (gen != g_gen) return;                         /* reset while this was out */
    if (local) g_local_busy[list] = 0;
    else       g_list_busy[list] = 0;
    if (!success || status < 200 || status >= 300 || !body) {
        PROV_LOG("tmdb: list %d failed (status %d)", list, status);
        if (status == 401)
            snprintf(g_error, sizeof g_error, "The TMDB key was not accepted - check it in Settings");
        return;
    }
    cJSON *root = cJSON_Parse(body);
    const cJSON *res = root ? cJSON_GetObjectItem(root, "results") : NULL;
    if (!res || !cJSON_IsArray(res)) {
        if (root) cJSON_Delete(root);
        return;
    }

    if (local) {
        /* The same titles in the TMDB language: what Home shows. */
        const cJSON *lo;
        int got = 0;
        cJSON_ArrayForEach(lo, res) {
            const cJSON *id = cJSON_GetObjectItem(lo, "id");
            if (!id || !cJSON_IsNumber(id)) continue;
            for (int j = 0; j < g_list_n[list]; ++j) {
                evo_tmdb_item_t *it = &g_list[list][j];
                if (it->id != id->valueint) continue;
                char t[128], ov[EVO_PROVIDER_MAX_OVERVIEW];
                json_s(lo, it->type == 'v' ? "title" : "name", t, sizeof t);
                evo_title_normalize_spaces(t);
                json_s(lo, "overview", ov, sizeof ov);
                snprintf(it->local_title, sizeof it->local_title, "%s", strcmp(t, it->title) ? t : "");
                if (ov[0]) snprintf(it->overview, sizeof it->overview, "%s", ov);
                got++;
            }
        }
        cJSON_Delete(root);
        PROV_LOG("tmdb: list %d in %s: %d titles", list, g_tag, got);
        return;
    }

    static evo_tmdb_item_t fresh[EVO_TMDB_PER_LIST];   /* 20 KB: not on the main thread's stack */
    memset(fresh, 0, sizeof fresh);
    int n = 0;
    const cJSON *o;
    cJSON_ArrayForEach(o, res) {
        if (n >= EVO_TMDB_PER_LIST) break;
        char media[16];
        json_s(o, "media_type", media, sizeof media);
        char type = (list == EVO_TMDB_POPULAR_FILMS || list == EVO_TMDB_NEW_FILMS) ? 'v'
                  : (list == EVO_TMDB_POPULAR_SERIES || list == EVO_TMDB_NEW_SERIES) ? 's'
                  : (strcmp(media, "movie") == 0) ? 'v' : (strcmp(media, "tv") == 0) ? 's' : 0;
        if (!type) continue;                          /* a person in the trending list */
        const cJSON *id = cJSON_GetObjectItem(o, "id");
        if (!id || !cJSON_IsNumber(id)) continue;

        evo_tmdb_item_t *it = &fresh[n];
        memset(it, 0, sizeof *it);
        it->id = id->valueint;
        it->type = type;
        json_s(o, type == 'v' ? "title" : "name", it->title, sizeof it->title);
        evo_title_normalize_spaces(it->title);          /* characters the console font lacks */
        json_s(o, type == 'v' ? "original_title" : "original_name", it->original, sizeof it->original);
        char date[16];
        json_s(o, type == 'v' ? "release_date" : "first_air_date", date, sizeof date);
        snprintf(it->year, sizeof it->year, "%.4s", date);
        json_s(o, "overview", it->overview, sizeof it->overview);
        {
            const cJSON *va = cJSON_GetObjectItem(o, "vote_average");
            const cJSON *vc = cJSON_GetObjectItem(o, "vote_count");
            if (va && cJSON_IsNumber(va) && va->valuedouble > 0.0 &&
                (!vc || !cJSON_IsNumber(vc) || vc->valueint >= 10)) {
                const int tenths = (int)(va->valuedouble * 10.0 + 0.5);
                snprintf(it->rating, sizeof it->rating, "%d.%d", tenths / 10, tenths % 10);
            }
        }
        char path[128];
        json_s(o, "poster_path", path, sizeof path);
        if (path[0] == '/') snprintf(it->poster, sizeof it->poster, "%s%s", TMDB_IMG, path);
        if (!it->title[0]) continue;

        /* Keep what an earlier fetch already found out. */
        for (int j = 0; j < g_list_n[list]; ++j) {
            const evo_tmdb_item_t *old = &g_list[list][j];
            if (old->id == it->id && old->type == it->type && (old->state || old->tgt)) {
                it->state = old->state;
                it->folder = old->folder;
                it->tgt = old->tgt;
                it->attempt = old->attempt;
                it->hits = old->hits;
                memcpy(it->key, old->key, sizeof it->key);
                memcpy(it->versions, old->versions, sizeof it->versions);
            }
        }
        n++;
    }
    cJSON_Delete(root);
    memcpy(g_list[list], fresh, sizeof fresh[0] * (size_t)n);
    g_list_n[list] = n;
    g_list_at[list] = time(NULL);
    g_error[0] = '\0';
    g_local_due[list] = strcmp(g_tag, "en-US") != 0;   /* then the same list in the TMDB language */
    PROV_LOG("tmdb: list %d: %d titles", list, n);
}

static void fetch_list(int list, int local)
{
    /* New films: out on streaming or disc (release types 4 and 5), newest
     * first, known enough to have votes - what a portal adds. */
    char today[16];
    {
        time_t t = time(NULL);
        struct tm tmv;
        localtime_r(&t, &tmv);
        snprintf(today, sizeof today, "%04d-%02d-%02d", tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday);
    }
    char newfilms[200];
    snprintf(newfilms, sizeof newfilms,
             "/discover/movie?sort_by=primary_release_date.desc&with_release_type=4%%7C5"
             "&release_date.lte=%s&vote_count.gte=40&include_adult=false", today);
    const char *const path[EVO_TMDB_LISTS] = {
        "/trending/all/week", "/movie/popular", "/tv/popular", newfilms, "/tv/on_the_air",
    };
    char url[EVO_TMDB_KEY_MAX + 400];
    const int v3 = key_is_v3();
    /* Matching with the portal needs the English titles; the TMDB language
     * comes in a second request, for what Home shows. */
    snprintf(url, sizeof url, TMDB_API "%s%clanguage=%s&page=1%s%s", path[list],
             strchr(path[list], '?') ? '&' : '?', local ? g_tag : "en-US",
             v3 ? "&api_key=" : "", v3 ? g_key : "");
    char auth[EVO_TMDB_KEY_MAX + 32];
    const char *hdr[2];
    int nh = 0;
    hdr[nh++] = "Accept: application/json";
    if (!v3) {
        snprintf(auth, sizeof auth, "Authorization: Bearer %s", g_key);
        hdr[nh++] = auth;
    }
    list_ctx_t *c = (list_ctx_t *)calloc(1, sizeof *c);
    if (!c) return;
    c->list = list;
    c->gen = g_gen;
    c->local = local;
    if (local) {
        g_local_busy[list] = 1;
        g_local_due[list] = 0;
    } else {
        g_list_busy[list] = 1;
        g_list_try[list] = time(NULL);
    }
    if (evo_net_request_async("GET", url, NULL, hdr, nh, on_list, c) != 0) {
        if (local) g_local_busy[list] = 0;
        else       g_list_busy[list] = 0;
        free(c);
    }
}

/* ------------------------------------------------------------------------- */
/* Looking a title up in the portal                                          */
/* ------------------------------------------------------------------------- */

typedef struct {
    unsigned gen;
    int      list;
    int      idx;
    int      id;
    char     type;
    int      tgt;
    char     provider[EVO_PROVIDER_MAX_ID];
} find_ctx_t;

/* Accents folded to plain letters ("Amélie" -> "Amelie"): portals often
 * write titles without them. 0 when another script is in it. */
static int ascii_fold(const char *in, char *out, size_t sz)
{
    size_t k = 0;
    for (const unsigned char *c = (const unsigned char *)in; *c && k + 1 < sz; ++c) {
        if (*c == 0xC3 && c[1]) {
            const char b = latin1_base(c[1]);
            ++c;
            out[k++] = b ? b : ' ';
            continue;
        }
        if (*c >= 0x80) {
            /* typographic quotes and dashes are fine; letters of other scripts are not */
            if (*c == 0xE2 && c[1] == 0x80 && c[2]) { out[k++] = (c[2] == 0x93 || c[2] == 0x94) ? '-' : '\''; c += 2; continue; }
            return 0;
        }
        out[k++] = (char)*c;
    }
    out[k] = '\0';
    return 1;
}

#define SEARCH_LAST 3

/* The search texts, in order: the title, the title without accents, the
 * original title, the part of the title before ":" or " - " (the portal may
 * write "Dune Part Two"). 0 when that one does not apply. */
static int search_text(const evo_tmdb_item_t *it, int attempt, char *out, size_t sz)
{
    char tmp[160];
    switch (attempt) {
    case 0:
        snprintf(out, sz, "%s", it->title);
        return 1;
    case 1:
        return ascii_fold(it->title, out, sz) && strcmp(out, it->title) != 0;
    case 2:
        if (!it->original[0] || strcmp(it->original, it->title) == 0) return 0;
        return ascii_fold(it->original, out, sz);
    case 3: {
        if (!ascii_fold(it->title, tmp, sizeof tmp)) snprintf(tmp, sizeof tmp, "%s", it->title);
        const char *cut = strchr(tmp, ':');
        const char *dash = strstr(tmp, " - ");
        if (!cut || (dash && dash < cut)) cut = dash;
        if (!cut || cut - tmp < 3) return 0;
        snprintf(out, sz, "%.*s", (int)(cut - tmp), tmp);
        return 1;
    }
    default:
        return 0;
    }
}

/* The provider of a source: "stalker#1" -> "stalker". */
static void tgt_provider(const char *tgt, char *out, size_t sz)
{
    snprintf(out, sz, "%s", tgt);
    char *h = strchr(out, '#');
    if (h) *h = '\0';
}

/* Every source titles can be looked up in, in a fixed order. */
static void build_targets(void)
{
    char sig[sizeof g_tgt_sig] = "";
    int n = 0;
    const evo_provider_t *p = evo_provider_find("stalker");
    if (p && p->list_catalog && p->is_configured && p->is_configured() && evo_provider_is_enabled("stalker")) {
        const int np = stalker_portal_count();
        for (int k = 0; k < np && n < MAX_TARGETS; ++k)
            snprintf(g_tgt[n++], sizeof g_tgt[0], "stalker#%d", k);
    }
    /* Emby and Jellyfin only while signed in: asking a server that wants a
     * sign-in would just fail every time. */
    static const char *const others[] = { "xtream", "iptv", "emby", "jellyfin" };
    for (int i = 0; i < 4 && n < MAX_TARGETS; ++i) {
        p = evo_provider_find(others[i]);
        if (p && p->list_catalog && p->is_configured && p->is_configured() && evo_provider_is_enabled(others[i]) &&
            !(p->needs_sign_in && p->needs_sign_in()))
            snprintf(g_tgt[n++], sizeof g_tgt[0], "%s", others[i]);
    }
    g_tgt_n = n;
    size_t sl = 0;
    for (int i = 0; i < n; ++i) {
        const int w = snprintf(sig + sl, sizeof sig - sl, "%s,", g_tgt[i]);
        if (w > 0 && (size_t)w < sizeof sig - sl) sl += (size_t)w;
    }
    if (strcmp(sig, g_tgt_sig) != 0) {
        /* Sources added or removed: look every title up again. */
        snprintf(g_tgt_sig, sizeof g_tgt_sig, "%s", sig);
        memset(g_tgt_fail, 0, sizeof g_tgt_fail);
        memset(g_tgt_down_until, 0, sizeof g_tgt_down_until);
        for (int l = 0; l < EVO_TMDB_LISTS; ++l)
            for (int i = 0; i < g_list_n[l]; ++i) {
                evo_tmdb_item_t *it = &g_list[l][i];
                it->state = 0;
                it->attempt = it->tgt = it->hits = 0;
                it->key[0] = it->versions[0] = '\0';
            }
        g_gen++;
        g_find_busy = 0;
    }
}

/* Add one version ("key US title") unless it is there already. */
static int add_version(char *vers, size_t sz, const char *key, const char *title)
{
    int n = 0;
    for (const char *c = vers; *c; ) {
        const char *us = strchr(c, '\x1f');
        const char *end = strchr(c, '\x1e');
        if (!end) end = c + strlen(c);
        if (us && us < end && (size_t)(us - c) == strlen(key) && strncmp(c, key, strlen(key)) == 0) return 0;
        n++;
        c = *end ? end + 1 : end;
    }
    if (n >= EVO_TMDB_MAX_VERSIONS) return 0;
    char t[96];
    snprintf(t, sizeof t, "%s", title);
    for (char *q = t; *q; ++q)
        if (*q == '\x1e' || *q == '\x1f' || *q == '\t' || *q == '\n' || *q == '\r') *q = ' ';
    const size_t vl = strlen(vers);
    const int w = snprintf(vers + vl, sz - vl, "%s%s\x1f%s", vl ? "\x1e" : "", key, t);
    if (w <= 0 || (size_t)w >= sz - vl) { vers[vl] = '\0'; return 0; }
    return 1;
}

/* Merge versions into the item; the first found becomes its key. */
static void item_merge(evo_tmdb_item_t *it, const char *vers, int folder)
{
    for (const char *c = vers; c && *c; ) {
        const char *us = strchr(c, '\x1f');
        const char *end = strchr(c, '\x1e');
        if (!end) end = c + strlen(c);
        if (us && us < end) {
            char key[sizeof it->key], title[128];
            snprintf(key, sizeof key, "%.*s", (int)(us - c), c);
            snprintf(title, sizeof title, "%.*s", (int)(end - us - 1), us + 1);
            if (add_version(it->versions, sizeof it->versions, key, title) && !it->key[0]) {
                snprintf(it->key, sizeof it->key, "%s", key);
                it->folder = folder;
            }
        }
        c = *end ? end + 1 : end;
    }
    if (it->key[0]) it->state = 1;
}

/* This source is done with the item: on to the next one. */
static void item_next_target(evo_tmdb_item_t *it)
{
    it->tgt++;
    it->attempt = 0;
    it->hits = 0;
    if (it->tgt >= g_tgt_n && !it->key[0]) it->state = 2;
}

/* What this source has of the item so far, as kept versions. */
static void target_versions(const evo_tmdb_item_t *it, const char *tgt, char *out, size_t sz)
{
    out[0] = '\0';
    char head[EVO_PROVIDER_MAX_ID + 24], prov[EVO_PROVIDER_MAX_ID + 8];
    tgt_provider(tgt, prov, sizeof prov);
    const char *hash = strchr(tgt, '#');
    if (hash) snprintf(head, sizeof head, "evo://%s/%d|", prov, atoi(hash + 1));   /* one portal */
    else      snprintf(head, sizeof head, "evo://%s/", prov);
    for (const char *c = it->versions; *c; ) {
        const char *end = strchr(c, '\x1e');
        if (!end) end = c + strlen(c);
        if (strncmp(c, head, strlen(head)) == 0) {
            const size_t ol = strlen(out);
            snprintf(out + ol, sz - ol, "%s%.*s", ol ? "\x1e" : "", (int)(end - c), c);
        }
        c = *end ? end + 1 : end;
    }
}

static void on_found(int ok, const evo_provider_item_t *items, int count, int more, void *ud)
{
    (void)more;
    find_ctx_t *c = (find_ctx_t *)ud;
    find_ctx_t f = *c;
    free(c);
    g_find_busy = 0;
    if (f.gen != g_gen || f.list >= EVO_TMDB_LISTS || f.idx >= g_list_n[f.list]) return;
    evo_tmdb_item_t *it = &g_list[f.list][f.idx];
    if (it->id != f.id || it->type != f.type || it->tgt != f.tgt) return;  /* changed meanwhile */
    if (f.tgt >= g_tgt_n) return;

    if (!ok) {                       /* the source did not answer */
        target_failed(f.tgt);
        return;
    }
    g_tgt_fail[f.tgt] = 0;
    const int film = it->type == 'v';
    char vers[EVO_TMDB_VERSIONS_SZ] = "";
    int folder = 0, got = 0;
    for (int i = 0; items && i < count; ++i) {
        const evo_provider_item_t *r = &items[i];
        if (film ? r->is_folder : !r->is_folder) continue;   /* a film is played, a series opened */
        if (evo_tmdb_same_title(r->title, r->subtitle, it->title, it->year, film) ||
            (it->original[0] && evo_tmdb_same_title(r->title, r->subtitle, it->original, it->year, film))) {
            char key[sizeof it->key];
            snprintf(key, sizeof key, "evo://%s/%s", f.provider, r->id);
            if (add_version(vers, sizeof vers, key, r->title)) got++;
            if (got == 1) folder = r->is_folder != 0;
        }
    }
    const int had = it->key[0] != 0;
    item_merge(it, vers, folder);
    it->hits += got;
    if (!had && it->key[0]) it->folder = folder;

    /* Every search text that applies (another spelling finds other versions);
     * the short "before the colon" one only when nothing turned up yet. */
    char next[160];
    it->attempt++;
    while (it->attempt <= SEARCH_LAST &&
           ((it->attempt == SEARCH_LAST && it->hits > 0) || !search_text(it, it->attempt, next, sizeof next)))
        it->attempt++;
    if (it->attempt > SEARCH_LAST) {
        char tid[16], mine[EVO_TMDB_VERSIONS_SZ];
        item_tid(it, tid, sizeof tid);
        target_versions(it, g_tgt[f.tgt], mine, sizeof mine);
        match_put(g_tgt[f.tgt], tid, mine[0] != 0, it->folder, "", mine);
        PROV_LOG("tmdb: '%s' in %s: %d version(s)", it->title, g_tgt[f.tgt], it->hits);
        item_next_target(it);
    }
}

static void find_next(void)
{
    match_load();
    const long now = (long)time(NULL);
    for (int l = 0; l < EVO_TMDB_LISTS; ++l) {
        for (int i = 0; i < g_list_n[l]; ++i) {
            evo_tmdb_item_t *it = &g_list[l][i];
            while (it->tgt < g_tgt_n) {
                if (g_tgt_down_until[it->tgt] > (time_t)now) {   /* not answering: go on without it */
                    item_next_target(it);
                    continue;
                }
                char tid[16];
                item_tid(it, tid, sizeof tid);
                const match_t *m = it->attempt == 0 ? match_find(g_tgt[it->tgt], tid) : NULL;
                if (m && now - m->when < (m->found ? FOUND_TTL : MISSING_TTL)) {
                    if (m->found) item_merge(it, m->versions, m->folder);
                    item_next_target(it);
                    continue;
                }
                /* The same title in another list, already done with this source. */
                const evo_tmdb_item_t *twin = NULL;
                for (int l2 = 0; l2 < EVO_TMDB_LISTS && !twin; ++l2)
                    for (int j = 0; j < g_list_n[l2]; ++j) {
                        const evo_tmdb_item_t *o = &g_list[l2][j];
                        if (o != it && o->id == it->id && o->type == it->type && o->tgt > it->tgt) {
                            twin = o;
                            break;
                        }
                    }
                if (twin && it->attempt == 0) {
                    char mine[EVO_TMDB_VERSIONS_SZ];
                    target_versions(twin, g_tgt[it->tgt], mine, sizeof mine);
                    item_merge(it, mine, twin->folder);
                    item_next_target(it);
                    continue;
                }
                break;
            }
            if (it->tgt >= g_tgt_n) continue;

            char prov[EVO_PROVIDER_MAX_ID + 8];
            tgt_provider(g_tgt[it->tgt], prov, sizeof prov);
            const evo_provider_t *p = evo_provider_find(prov);
            char text[160], q[260];
            if (!p || !p->list_catalog || it->attempt > SEARCH_LAST ||
                !search_text(it, it->attempt, text, sizeof text)) {
                item_next_target(it);                /* nothing (left) to search for here */
                return;                              /* next frame goes on */
            }
            const char *hash = strchr(g_tgt[it->tgt], '#');
            if (hash) snprintf(q, sizeof q, "@findin:%d:%c:%s", atoi(hash + 1), it->type, text);
            else      snprintf(q, sizeof q, EVO_PROVIDER_FIND "%c:%s", it->type, text);
            find_ctx_t *c = (find_ctx_t *)calloc(1, sizeof *c);
            if (!c) return;
            c->gen = g_gen;
            c->list = l;
            c->idx = i;
            c->id = it->id;
            c->type = it->type;
            c->tgt = it->tgt;
            snprintf(c->provider, sizeof c->provider, "%s", prov);
            g_find_busy = 1;
            PROV_LOG("tmdb: looking up '%s' in %s", text, g_tgt[it->tgt]);   /* the last line before a crash says where */
            const int t = it->tgt;
            if (p->list_catalog(q, 0, on_found, c) != 0 && g_find_busy) {
                /* Refused without an answer (not signed in, no network): the
                 * reply will not come. */
                free(c);
                g_find_busy = 0;
                target_failed(t);
            }
            return;                                  /* one at a time */
        }
    }
}

/* ------------------------------------------------------------------------- */
/* Public                                                                    */
/* ------------------------------------------------------------------------- */

void evo_tmdb_clear_cache(void)
{
    match_load();
    g_match_n = 0;
    g_match_dirty = 0;
    remove(evo_data_path(TMDB_MATCH));
    for (int l = 0; l < EVO_TMDB_LISTS; ++l)
        for (int i = 0; i < g_list_n[l]; ++i) {
            evo_tmdb_item_t *it = &g_list[l][i];
            it->state = 0;
            it->attempt = it->tgt = it->hits = 0;
            it->key[0] = it->versions[0] = '\0';
        }
    g_gen++;
    g_find_busy = 0;
    g_find_pause_until = 0;
    memset(g_list_at, 0, sizeof g_list_at);           /* the lists themselves again too */
    memset(g_list_try, 0, sizeof g_list_try);
    PROV_LOG("tmdb: cache cleared");
}

void evo_tmdb_reset(void)
{
    g_gen++;
    memset(g_list_n, 0, sizeof g_list_n);
    memset(g_list_at, 0, sizeof g_list_at);
    memset(g_list_try, 0, sizeof g_list_try);
    memset(g_list_busy, 0, sizeof g_list_busy);
    memset(g_local_busy, 0, sizeof g_local_busy);
    memset(g_local_due, 0, sizeof g_local_due);
    g_find_busy = 0;
    g_find_pause_until = 0;
    g_error[0] = '\0';
}

void evo_tmdb_tick(const char *provider_id)
{
    (void)provider_id;
    key_load();
    if (!g_key[0]) return;
    build_targets();
    if (g_tgt_n == 0) return;
    const time_t now = time(NULL);
    {
        /* Another TMDB language: fetch the lists again (what was found stays). */
        const char *tag = lang_tag();
        if (strcmp(tag, g_tag) != 0) {
            snprintf(g_tag, sizeof g_tag, "%s", tag);
            for (int l = 0; l < EVO_TMDB_LISTS; ++l) {
                g_list_at[l] = 0;
                g_list_try[l] = 0;
                for (int i = 0; i < g_list_n[l]; ++i) g_list[l][i].local_title[0] = '\0';
            }
        }
    }
    for (int l = 0; l < EVO_TMDB_LISTS; ++l) {
        if (g_list_busy[l]) continue;
        const int due = !g_list_at[l] || now - g_list_at[l] >= LIST_TTL;
        const int may = !g_list_try[l] || now - g_list_try[l] >= LIST_RETRY;
        if (due && may) { fetch_list(l, 0); continue; }
        if (g_local_due[l] && !g_local_busy[l]) fetch_list(l, 1);
    }
    if (!g_find_busy && now >= g_find_pause_until) find_next();
    /* The whole file (up to 2 MB) used to be rewritten after every lookup. */
    if (g_match_dirty && now - g_match_saved >= 5) {
        match_save();
        g_match_dirty = 0;
        g_match_saved = now;
    }
}

int evo_tmdb_list(int list, const evo_tmdb_item_t **items)
{
    if (list < 0 || list >= EVO_TMDB_LISTS) { if (items) *items = NULL; return 0; }
    if (items) *items = g_list[list];
    return g_list_n[list];
}

int evo_tmdb_version_count(const evo_tmdb_item_t *it)
{
    if (!it || !it->versions[0]) return (it && it->key[0]) ? 1 : 0;
    int n = 1;
    for (const char *c = it->versions; *c; ++c)
        if (*c == '\x1e') n++;
    return n;
}

int evo_tmdb_version(const evo_tmdb_item_t *it, int i, char *key, size_t key_sz, char *title, size_t title_sz)
{
    if (!it) return 0;
    if (!it->versions[0]) {                       /* found before versions were kept */
        if (i != 0 || !it->key[0]) return 0;
        snprintf(key, key_sz, "%s", it->key);
        snprintf(title, title_sz, "%s", it->title);
        return 1;
    }
    const char *c = it->versions;
    for (int k = 0; k < i && c; ++k) {
        c = strchr(c, '\x1e');
        if (c) ++c;
    }
    if (!c || !*c) return 0;
    const char *us = strchr(c, '\x1f');
    const char *end = strchr(c, '\x1e');
    if (!end) end = c + strlen(c);
    if (!us || us > end) return 0;
    snprintf(key, key_sz, "%.*s", (int)(us - c), c);
    snprintf(title, title_sz, "%.*s", (int)(end - us - 1), us + 1);
    return 1;
}

int evo_tmdb_take_error(char *out, size_t sz)
{
    if (!g_error[0]) return 0;
    snprintf(out, sz, "%s", g_error);
    g_error[0] = '\0';
    return 1;
}
