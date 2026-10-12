/*
 * evo_opensubs.c - OpenSubtitles.com REST client. See evo_opensubs.h.
 */
#include "evo_opensubs.h"
#include "evo_subtitle.h"      /* prospero_opensub_key() */
#include "evo_data_path.h"
#include "evo_net.h"
#include "cJSON.h"
#include "evo_title_tag.h"   /* labels and odd characters out of a portal title */
#include "evo_boot_log.h"

#include <ctype.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#ifndef EVO_PLAYER_VERSION
#define EVO_PLAYER_VERSION "v0.12.0"
#endif

#define OSUB_MAX_RESULTS 40
#define OSUB_TOKEN_MAX   1024
#define OSUB_HOST_MAX    96
#define OSUB_USER_MAX    96
#define OSUB_PASS_MAX    96

typedef struct {
    long file_id;
    char lang[8];
    char release[120];
    long downloads;
    int  hearing_impaired;
} osub_result_t;

static const char *k_lang_label[EVO_OSUB_LANG_COUNT] = {
    "NEDERLANDS", "ENGLISH", "NEDERLANDS + ENGLISH", "DEUTSCH",
    "FRANCAIS", "ESPANOL", "TURKCE", "ARABIC"
};
static const char *k_lang_code[EVO_OSUB_LANG_COUNT] = {
    "nl", "en", "nl,en", "de", "fr", "es", "tr", "ar"
};

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static evo_osub_state_t g_state = OSUB_IDLE;
static char   g_message[200] = "";
static osub_result_t g_results[OSUB_MAX_RESULTS];
static int    g_result_count = 0;
static char   g_file[600] = "";
static unsigned g_gen = 0;                 /* bumped by reset: a late reply is dropped */
static int    g_busy = 0;                  /* a worker is running */

/* settings */
static int  g_cfg_loaded = 0;
static int  g_lang = 0;
static char g_user[OSUB_USER_MAX] = "";
static char g_pass[OSUB_PASS_MAX] = "";

/* session (worker thread only, plus the cached token) */
static char g_token[OSUB_TOKEN_MAX] = "";
static char g_host[OSUB_HOST_MAX] = "api.opensubtitles.com";

/* the request in flight */
static char g_media[512] = "";
static char g_hint[200] = "";
static int  g_download_index = -1;

/* ---------------------------------------------------------------------- */
/* Settings                                                                */
/* ---------------------------------------------------------------------- */

static void cfg_strip(char *s)
{
    size_t n = strlen(s);
    while (n && (s[n - 1] == '\n' || s[n - 1] == '\r')) s[--n] = '\0';
}

static void cfg_load(void)
{
    if (g_cfg_loaded) return;
    g_cfg_loaded = 1;
    FILE *f = fopen(evo_data_path("evo_opensubtitles.cfg"), "r");
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof line, f)) {
        cfg_strip(line);
        if (strncmp(line, "lang=", 5) == 0) {
            int v = atoi(line + 5);
            if (v >= 0 && v < EVO_OSUB_LANG_COUNT) g_lang = v;
        } else if (strncmp(line, "user=", 5) == 0) {
            snprintf(g_user, sizeof g_user, "%s", line + 5);
        } else if (strncmp(line, "pass=", 5) == 0) {
            snprintf(g_pass, sizeof g_pass, "%s", line + 5);
        }
    }
    fclose(f);
}

static int cfg_save(void)
{
    FILE *f = fopen(evo_data_path("evo_opensubtitles.cfg"), "w");
    if (!f) return 0;
    fprintf(f, "lang=%d\nuser=%s\npass=%s\n", g_lang, g_user, g_pass);
    fclose(f);
    return 1;
}

const char *evo_osub_lang_label(int index)
{
    return (index >= 0 && index < EVO_OSUB_LANG_COUNT) ? k_lang_label[index] : "";
}

int evo_osub_lang_get(void) { cfg_load(); return g_lang; }

int evo_osub_lang_set(int index)
{
    cfg_load();
    if (index < 0 || index >= EVO_OSUB_LANG_COUNT) return 0;
    g_lang = index;
    return cfg_save();
}

const char *evo_osub_user(void) { cfg_load(); return g_user; }
int evo_osub_has_password(void) { cfg_load(); return g_pass[0] != '\0'; }

static int one_line_ok(const char *s)
{
    for (; *s; ++s)
        if (*s == '\n' || *s == '\r') return 0;
    return 1;
}

int evo_osub_set_user(const char *user)
{
    cfg_load();
    char tmp[OSUB_USER_MAX];
    snprintf(tmp, sizeof tmp, "%s", user ? user : "");
    cfg_strip(tmp);
    if (!one_line_ok(tmp)) return 0;
    snprintf(g_user, sizeof g_user, "%s", tmp);
    g_token[0] = '\0';                    /* a new login needs a new token */
    return cfg_save();
}

int evo_osub_set_password(const char *pw)
{
    cfg_load();
    char tmp[OSUB_PASS_MAX];
    snprintf(tmp, sizeof tmp, "%s", pw ? pw : "");
    cfg_strip(tmp);
    if (!one_line_ok(tmp)) return 0;
    snprintf(g_pass, sizeof g_pass, "%s", tmp);
    g_token[0] = '\0';
    return cfg_save();
}

/* ---------------------------------------------------------------------- */
/* State helpers                                                           */
/* ---------------------------------------------------------------------- */

evo_osub_state_t evo_osub_state(void)
{
    pthread_mutex_lock(&g_mu);
    evo_osub_state_t s = g_state;
    pthread_mutex_unlock(&g_mu);
    return s;
}

const char *evo_osub_message(void)
{
    static char copy[200];
    pthread_mutex_lock(&g_mu);
    snprintf(copy, sizeof copy, "%s", g_message);
    pthread_mutex_unlock(&g_mu);
    return copy;
}

int evo_osub_result_count(void)
{
    pthread_mutex_lock(&g_mu);
    int n = g_result_count;
    pthread_mutex_unlock(&g_mu);
    return n;
}

const char *evo_osub_file(void) { return g_file; }

void evo_osub_result_text(int index, char *label, size_t label_cap,
                          char *detail, size_t detail_cap)
{
    if (label && label_cap) label[0] = '\0';
    if (detail && detail_cap) detail[0] = '\0';
    pthread_mutex_lock(&g_mu);
    if (index >= 0 && index < g_result_count) {
        const osub_result_t *r = &g_results[index];
        char lang[8];
        size_t i = 0;
        for (; r->lang[i] && i < sizeof lang - 1; ++i) lang[i] = (char)toupper((unsigned char)r->lang[i]);
        lang[i] = '\0';
        if (label && label_cap)
            snprintf(label, label_cap, "[%s] %s", lang[0] ? lang : "??",
                     r->release[0] ? r->release : "subtitle");
        if (detail && detail_cap)
            snprintf(detail, detail_cap, "%ld DOWNLOADS%s", r->downloads,
                     r->hearing_impaired ? "  -  HEARING IMPAIRED" : "");
    }
    pthread_mutex_unlock(&g_mu);
}

static void set_state(unsigned gen, evo_osub_state_t s, const char *msg)
{
    pthread_mutex_lock(&g_mu);
    if (gen == g_gen) {
        g_state = s;
        snprintf(g_message, sizeof g_message, "%s", msg ? msg : "");
    }
    pthread_mutex_unlock(&g_mu);
}

void evo_osub_reset(void)
{
    pthread_mutex_lock(&g_mu);
    ++g_gen;
    g_state = OSUB_IDLE;
    g_message[0] = '\0';
    g_result_count = 0;
    pthread_mutex_unlock(&g_mu);
}

/* ---------------------------------------------------------------------- */
/* Name -> query                                                           */
/* ---------------------------------------------------------------------- */

static int is_year(const char *t)
{
    if (strlen(t) != 4) return 0;
    for (int i = 0; i < 4; ++i) if (!isdigit((unsigned char)t[i])) return 0;
    int y = atoi(t);
    return y >= 1900 && y <= 2099;
}

static int is_sxxexx(const char *t, int *s, int *e)
{
    int si, ei, n = 0;
    if ((t[0] == 's' || t[0] == 'S') && sscanf(t + 1, "%d%*[eE]%d%n", &si, &ei, &n) == 2 && n > 0) {
        *s = si; *e = ei;
        return 1;
    }
    return 0;
}

static int is_junk(const char *t)
{
    static const char *junk[] = {
        "480p", "576p", "720p", "1080p", "2160p", "4k", "uhd", "hdr", "hdr10", "dv",
        "bluray", "blu-ray", "bdrip", "brrip", "webrip", "web-dl", "webdl", "web", "hdtv",
        "dvdrip", "remux", "x264", "x265", "h264", "h265", "hevc", "avc", "av1",
        "proper", "repack", "extended", "unrated", "multi", NULL
    };
    for (int i = 0; junk[i]; ++i)
        if (strcasecmp(t, junk[i]) == 0) return 1;
    return 0;
}

void evo_osub_parse_name(const char *media_path, char *query, size_t cap,
                         int *year, int *season, int *episode)
{
    if (query && cap) query[0] = '\0';
    if (year) *year = 0;
    if (season) *season = -1;
    if (episode) *episode = -1;
    if (!media_path || !query || !cap) return;

    const char *base = strrchr(media_path, '/');
    base = base ? base + 1 : media_path;

    char name[256];
    snprintf(name, sizeof name, "%s", base);
    char *dot = strrchr(name, '.');
    if (dot && strlen(dot) <= 5) *dot = '\0';          /* the extension */

    for (char *p = name; *p; ++p)
        if (*p == '.' || *p == '_') *p = ' ';

    char out[256] = "";
    char *save = NULL;
    for (char *tok = strtok_r(name, " ", &save); tok; tok = strtok_r(NULL, " ", &save)) {
        int s, e;
        if (is_sxxexx(tok, &s, &e)) {
            if (season) *season = s;
            if (episode) *episode = e;
            break;                                     /* the title is what came before */
        }
        if (is_year(tok) && out[0]) {
            if (year) *year = atoi(tok);
            break;
        }
        if (is_junk(tok) && out[0]) break;
        if (tok[0] == '[' || tok[0] == '(') {
            if (out[0]) break;
            continue;
        }
        if (out[0]) strncat(out, " ", sizeof out - strlen(out) - 1);
        strncat(out, tok, sizeof out - strlen(out) - 1);
    }
    snprintf(query, cap, "%s", out);
}


/*
 * A title the provider gave, not a file name: "MULTI Scrubs (2026) - S1E2",
 * "MULTI Blade: Trinity 4K - 2004", "NL - Flikken Maastricht - S12E3".
 * The label goes (evo_title_tag.h), the episode code and the year come out,
 * quality words go, and what is left is the title to search for.
 */
void evo_osub_parse_title(const char *title, char *query, size_t cap,
                          int *year, int *season, int *episode)
{
    if (query && cap) query[0] = '\0';
    if (year) *year = 0;
    if (season) *season = -1;
    if (episode) *episode = -1;
    if (!title || !query || !cap) return;

    char t[256];
    evo_title_strip_tags(title, t, sizeof t);

    /* " - S1E2" / " - E5" / " S01E02" at the end */
    char *dash = NULL;
    for (char *p = strstr(t, " - "); p; p = strstr(p + 1, " - ")) dash = p;
    char *code = dash ? dash + 3 : NULL;
    if (!code) { code = strrchr(t, ' '); if (code) code++; }
    if (code) {
        int s = -1, e = -1, n = 0;
        if ((code[0] == 'S' || code[0] == 's') &&
            sscanf(code + 1, "%d%*[eE]%d%n", &s, &e, &n) == 2 && code[1 + n] == '\0') {
            if (season) *season = s;
            if (episode) *episode = e;
            code[-1] = '\0';
            if (dash && code == dash + 3) *dash = '\0';
        } else if ((code[0] == 'E' || code[0] == 'e') && sscanf(code + 1, "%d%n", &e, &n) == 1 &&
                   code[1 + n] == '\0') {
            if (season) *season = 1;                  /* a series without seasons */
            if (episode) *episode = e;
            code[-1] = '\0';
            if (dash && code == dash + 3) *dash = '\0';
        } else if (dash && code == dash + 3 && is_year(code)) {     /* "... - 2004" */
            if (year) *year = atoi(code);
            *dash = '\0';
        }
    }

    /* "(2026)" anywhere: the year, and out of the query */
    for (char *p = strchr(t, '('); p; p = strchr(p + 1, '(')) {
        if (isdigit((unsigned char)p[1]) && isdigit((unsigned char)p[2]) && isdigit((unsigned char)p[3]) &&
            isdigit((unsigned char)p[4]) && p[5] == ')') {
            char y[5] = { p[1], p[2], p[3], p[4], 0 };
            if (is_year(y)) {
                if (year && !*year) *year = atoi(y);
                memmove(p, p + 6, strlen(p + 6) + 1);
                break;
            }
        }
    }

    /* quality words ("4K", "1080p", "HDR") and stray separators go */
    char out[256] = "";
    char *save = NULL;
    for (char *tok = strtok_r(t, " ", &save); tok; tok = strtok_r(NULL, " ", &save)) {
        if (is_junk(tok) || strcmp(tok, "-") == 0 || strcmp(tok, "|") == 0) continue;
        if (out[0]) strncat(out, " ", sizeof out - strlen(out) - 1);
        strncat(out, tok, sizeof out - strlen(out) - 1);
    }
    size_t n = strlen(out);
    while (n && (out[n - 1] == ' ' || out[n - 1] == '-' || out[n - 1] == ':')) out[--n] = '\0';
    snprintf(query, cap, "%s", out);
}

/* ---------------------------------------------------------------------- */
/* HTTP                                                                    */
/* ---------------------------------------------------------------------- */

static void url_encode(const char *in, char *out, size_t cap)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t o = 0;
    for (; *in && o + 4 < cap; ++in) {
        unsigned char c = (unsigned char)*in;
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') out[o++] = (char)c;
        else { out[o++] = '%'; out[o++] = hex[c >> 4]; out[o++] = hex[c & 15]; }
    }
    out[o] = '\0';
}

static const char *http_reason(int rc, int status)
{
    if (status == 401) return "Login or API key refused";
    if (status == 403) return "Access refused - check the API key";
    if (status == 404) return "Not found";
    if (status == 406) return "Download limit reached for today";
    if (status == 429) return "Too many requests - try again in a moment";
    if (status >= 500) return "OpenSubtitles is not answering";
    if (rc == EVO_NET_ERR_DNS) return "Could not find opensubtitles.com";
    if (rc == EVO_NET_ERR_CONNECT) return "Could not connect - check the network";
    if (rc <= EVO_NET_ERR_SSL_CTX && rc >= EVO_NET_ERR_SSL_CONN) return "Secure connection failed";
    if (rc == EVO_NET_ERR_BODY_LIMIT) return "Reply too large";
    return "The request failed";
}

/* On success returns 1 with *body owned by the caller. On failure returns 0
 * and puts a sentence in why. */
static int api_call(int post, const char *path_query, const char *body_in, int with_token,
                    char **out, char *why, size_t why_cap)
{
    const char *key = prospero_opensub_key();
    char url[EVO_NET_MAX_URL];
    snprintf(url, sizeof url, "https://%s%s", g_host, path_query);

    char h_key[160], h_ua[96], h_auth[OSUB_TOKEN_MAX + 32];
    snprintf(h_key, sizeof h_key, "Api-Key: %s", key);
    snprintf(h_ua, sizeof h_ua, "User-Agent: EVOPlayer %s", EVO_PLAYER_VERSION);
    const char *hdr[5];
    int n = 0;
    hdr[n++] = h_key;
    hdr[n++] = h_ua;
    hdr[n++] = "Content-Type: application/json";
    hdr[n++] = "Accept: application/json";
    if (with_token) {
        snprintf(h_auth, sizeof h_auth, "Authorization: Bearer %s", g_token);
        hdr[n++] = h_auth;
    }

    char *resp = NULL;
    size_t len = 0;
    int status = 0;
    int rc = post ? evo_net_http_post_sync(url, body_in ? body_in : "{}", hdr, n, &resp, &len, &status)
                  : evo_net_http_get_sync(url, hdr, n, &resp, &len, &status);
    if (rc != EVO_NET_OK || status < 200 || status >= 300) {
        snprintf(why, why_cap, "%s", http_reason(rc, status));
        free(resp);
        return 0;
    }
    *out = resp;
    return 1;
}

static int do_login(char *why, size_t why_cap)
{
    cfg_load();
    if (!g_user[0] || !g_pass[0]) {
        snprintf(why, why_cap, "Set the OpenSubtitles username and password in Settings");
        return 0;
    }
    if (g_token[0]) return 1;

    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "username", g_user);
    cJSON_AddStringToObject(req, "password", g_pass);
    char *body = cJSON_PrintUnformatted(req);
    cJSON_Delete(req);
    if (!body) { snprintf(why, why_cap, "Out of memory"); return 0; }

    char *resp = NULL;
    snprintf(g_host, sizeof g_host, "api.opensubtitles.com");
    int ok = api_call(1, "/api/v1/login", body, 0, &resp, why, why_cap);
    free(body);
    if (!ok) return 0;

    cJSON *root = cJSON_Parse(resp);
    free(resp);
    const cJSON *tok = root ? cJSON_GetObjectItemCaseSensitive(root, "token") : NULL;
    if (!cJSON_IsString(tok) || !tok->valuestring[0]) {
        cJSON_Delete(root);
        snprintf(why, why_cap, "Login failed");
        return 0;
    }
    snprintf(g_token, sizeof g_token, "%s", tok->valuestring);
    const cJSON *base = cJSON_GetObjectItemCaseSensitive(root, "base_url");
    if (cJSON_IsString(base) && base->valuestring[0]) {
        /* "api.opensubtitles.com" - never a path; refuse anything odd. */
        const char *b = base->valuestring;
        if (strncmp(b, "https://", 8) == 0) b += 8;
        int clean = 1;
        for (const char *c = b; *c; ++c)
            if (!(isalnum((unsigned char)*c) || *c == '.' || *c == '-')) clean = 0;
        if (clean && strstr(b, "opensubtitles.com")) snprintf(g_host, sizeof g_host, "%s", b);
    }
    cJSON_Delete(root);
    return 1;
}

/* ---------------------------------------------------------------------- */
/* Search                                                                  */
/* ---------------------------------------------------------------------- */

static int parse_results(const char *json)
{
    cJSON *root = cJSON_Parse(json);
    const cJSON *data = root ? cJSON_GetObjectItemCaseSensitive(root, "data") : NULL;
    int n = 0;
    if (cJSON_IsArray(data)) {
        const cJSON *item;
        pthread_mutex_lock(&g_mu);
        cJSON_ArrayForEach(item, data) {
            if (n >= OSUB_MAX_RESULTS) break;
            const cJSON *at = cJSON_GetObjectItemCaseSensitive(item, "attributes");
            if (!at) continue;
            const cJSON *files = cJSON_GetObjectItemCaseSensitive(at, "files");
            const cJSON *f0 = cJSON_IsArray(files) ? cJSON_GetArrayItem(files, 0) : NULL;
            const cJSON *fid = f0 ? cJSON_GetObjectItemCaseSensitive(f0, "file_id") : NULL;
            if (!cJSON_IsNumber(fid)) continue;

            osub_result_t *r = &g_results[n];
            memset(r, 0, sizeof *r);
            r->file_id = (long)fid->valuedouble;
            const cJSON *lang = cJSON_GetObjectItemCaseSensitive(at, "language");
            if (cJSON_IsString(lang)) snprintf(r->lang, sizeof r->lang, "%s", lang->valuestring);
            const cJSON *rel = cJSON_GetObjectItemCaseSensitive(at, "release");
            if (cJSON_IsString(rel) && rel->valuestring[0])
                snprintf(r->release, sizeof r->release, "%s", rel->valuestring);
            else {
                const cJSON *fn = cJSON_GetObjectItemCaseSensitive(f0, "file_name");
                if (cJSON_IsString(fn)) snprintf(r->release, sizeof r->release, "%s", fn->valuestring);
            }
            const cJSON *dl = cJSON_GetObjectItemCaseSensitive(at, "download_count");
            if (cJSON_IsNumber(dl)) r->downloads = (long)dl->valuedouble;
            const cJSON *hi = cJSON_GetObjectItemCaseSensitive(at, "hearing_impaired");
            r->hearing_impaired = cJSON_IsTrue(hi);
            ++n;
        }
        g_result_count = n;
        pthread_mutex_unlock(&g_mu);
    }
    cJSON_Delete(root);
    return n;
}

static void *search_worker(void *arg)
{
    unsigned gen = (unsigned)(size_t)arg;
    char why[160] = "";

    if (!prospero_opensub_key()[0]) {
        set_state(gen, OSUB_FAILED, "Set the OpenSubtitles API key in Settings first");
        goto done;
    }
    set_state(gen, OSUB_WORKING, "SIGNING IN...");
    if (!do_login(why, sizeof why)) { set_state(gen, OSUB_FAILED, why); goto done; }

    char query[200];
    int year = 0, season = -1, episode = -1;
    /*
     * A stream from a portal plays from a URL like ".../movie.php?mac=..&
     * stream=2022646.mkv&play_token=..": read as a file name that searched for
     * "movie php mac ..." and listed whatever matched. For anything that is not
     * a local file, the provider's title is what names the film or episode.
     */
    const int is_stream = strstr(g_media, "://") != NULL;
    if (!is_stream)
        evo_osub_parse_name(g_media, query, sizeof query, &year, &season, &episode);
    if (!query[0] && g_hint[0])
        evo_osub_parse_title(g_hint, query, sizeof query, &year, &season, &episode);
    if (!query[0] && g_hint[0]) snprintf(query, sizeof query, "%s", g_hint);
    if (!query[0]) { set_state(gen, OSUB_FAILED, "No title to search for"); goto done; }

    /* The API wants lower-case parameters in alphabetical order. */
    char enc[300], langs[40], path[700];
    url_encode(query, enc, sizeof enc);
    url_encode(k_lang_code[g_lang], langs, sizeof langs);
    int len = snprintf(path, sizeof path, "/api/v1/subtitles?");
    if (episode >= 0) len += snprintf(path + len, sizeof path - len, "episode_number=%d&", episode);
    len += snprintf(path + len, sizeof path - len, "languages=%s&order_by=download_count&query=%s", langs, enc);
    if (season >= 0) len += snprintf(path + len, sizeof path - len, "&season_number=%d", season);
    len += snprintf(path + len, sizeof path - len, "&type=%s", season >= 0 ? "episode" : "movie");
    if (year > 0 && season < 0) len += snprintf(path + len, sizeof path - len, "&year=%d", year);
    evo_boot_log("opensubtitles: searching \"%s\" year=%d season=%d episode=%d",
                 query, year, season, episode);

    set_state(gen, OSUB_WORKING, "SEARCHING...");
    char *resp = NULL;
    if (!api_call(0, path, NULL, 1, &resp, why, sizeof why)) {
        if (strstr(why, "Login or API key")) g_token[0] = '\0';   /* maybe the token expired */
        set_state(gen, OSUB_FAILED, why);
        goto done;
    }
    int n = parse_results(resp);
    free(resp);
    if (n <= 0) { set_state(gen, OSUB_FAILED, "No subtitles found"); goto done; }

    char msg[64];
    snprintf(msg, sizeof msg, "%d RESULTS", n);
    set_state(gen, OSUB_RESULTS, msg);

done:
    pthread_mutex_lock(&g_mu);
    g_busy = 0;
    pthread_mutex_unlock(&g_mu);
    return NULL;
}

static int start_worker(void *(*fn)(void *), unsigned gen)
{
    pthread_t t;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_attr_setstacksize(&attr, 512 * 1024);
    int rc = pthread_create(&t, &attr, fn, (void *)(size_t)gen);
    pthread_attr_destroy(&attr);
    return rc == 0;
}

void evo_osub_search_start(const char *media_path, const char *title_hint)
{
    pthread_mutex_lock(&g_mu);
    if (g_busy) { pthread_mutex_unlock(&g_mu); return; }
    g_busy = 1;
    ++g_gen;
    unsigned gen = g_gen;
    g_state = OSUB_WORKING;
    snprintf(g_message, sizeof g_message, "STARTING...");
    g_result_count = 0;
    snprintf(g_media, sizeof g_media, "%s", media_path ? media_path : "");
    snprintf(g_hint, sizeof g_hint, "%s", title_hint ? title_hint : "");
    pthread_mutex_unlock(&g_mu);

    if (!start_worker(search_worker, gen)) {
        pthread_mutex_lock(&g_mu);
        g_busy = 0;
        pthread_mutex_unlock(&g_mu);
        set_state(gen, OSUB_FAILED, "Could not start the search");
    }
}

/* ---------------------------------------------------------------------- */
/* Download                                                                */
/* ---------------------------------------------------------------------- */

static int file_exists(const char *p)
{
    FILE *f = fopen(p, "rb");
    if (!f) return 0;
    fclose(f);
    return 1;
}

/* Where the file goes: next to the video as <name>.srt, so it is found by
 * itself next time - or <name>.<lang>.srt when that name is taken, or the data
 * directory when the video's folder cannot be written (a stream, a read-only
 * drive). */
static void choose_target(const char *lang, char *out, size_t cap)
{
    out[0] = '\0';
    if (g_media[0]) {
        char base[512];
        snprintf(base, sizeof base, "%s", g_media);
        char *slash = strrchr(base, '/');
        char *dot = strrchr(base, '.');
        if (dot && (!slash || dot > slash)) *dot = '\0';

        char cand[560];
        snprintf(cand, sizeof cand, "%s.srt", base);
        if (file_exists(cand)) snprintf(cand, sizeof cand, "%s.%s.srt", base, lang[0] ? lang : "sub");
        FILE *probe = fopen(cand, "ab");              /* writable? a failed probe leaves nothing */
        if (probe) {
            fclose(probe);
            snprintf(out, cap, "%s", cand);
            return;
        }
    }
    snprintf(out, cap, "%s", evo_data_path("evo_sub_download.srt"));
}

static void *download_worker(void *arg)
{
    unsigned gen = (unsigned)(size_t)arg;
    char why[160] = "";
    char lang[8] = "";
    long file_id = 0;

    pthread_mutex_lock(&g_mu);
    if (g_download_index >= 0 && g_download_index < g_result_count) {
        file_id = g_results[g_download_index].file_id;
        snprintf(lang, sizeof lang, "%s", g_results[g_download_index].lang);
    }
    pthread_mutex_unlock(&g_mu);
    if (!file_id) { set_state(gen, OSUB_FAILED, "That result is gone"); goto done; }

    set_state(gen, OSUB_WORKING, "SIGNING IN...");
    if (!do_login(why, sizeof why)) { set_state(gen, OSUB_FAILED, why); goto done; }

    set_state(gen, OSUB_WORKING, "DOWNLOADING...");
    char body[96];
    snprintf(body, sizeof body, "{\"file_id\":%ld,\"sub_format\":\"srt\"}", file_id);
    char *resp = NULL;
    if (!api_call(1, "/api/v1/download", body, 1, &resp, why, sizeof why)) {
        if (strstr(why, "Login or API key")) g_token[0] = '\0';
        set_state(gen, OSUB_FAILED, why);
        goto done;
    }
    cJSON *root = cJSON_Parse(resp);
    free(resp);
    const cJSON *link = root ? cJSON_GetObjectItemCaseSensitive(root, "link") : NULL;
    if (!cJSON_IsString(link) || strncmp(link->valuestring, "http", 4) != 0) {
        cJSON_Delete(root);
        set_state(gen, OSUB_FAILED, "No download link came back");
        goto done;
    }
    char url[EVO_NET_MAX_URL];
    snprintf(url, sizeof url, "%s", link->valuestring);
    cJSON_Delete(root);

    char h_ua[96];
    snprintf(h_ua, sizeof h_ua, "User-Agent: EVOPlayer %s", EVO_PLAYER_VERSION);
    const char *hdr[1] = { h_ua };
    char *srt = NULL;
    size_t srt_len = 0;
    int status = 0;
    int rc = evo_net_http_get_sync(url, hdr, 1, &srt, &srt_len, &status);
    if (rc != EVO_NET_OK || !srt || srt_len < 16 || status < 200 || status >= 300) {
        snprintf(why, sizeof why, "%s", http_reason(rc, status));
        free(srt);
        set_state(gen, OSUB_FAILED, why);
        goto done;
    }

    char target[560];
    choose_target(lang, target, sizeof target);
    FILE *f = fopen(target, "wb");
    if (!f) {
        free(srt);
        set_state(gen, OSUB_FAILED, "Could not save the subtitle file");
        goto done;
    }
    size_t wrote = fwrite(srt, 1, srt_len, f);
    fclose(f);
    free(srt);
    if (wrote != srt_len) { set_state(gen, OSUB_FAILED, "Could not save the subtitle file"); goto done; }

    pthread_mutex_lock(&g_mu);
    snprintf(g_file, sizeof g_file, "%s", target);
    pthread_mutex_unlock(&g_mu);
    set_state(gen, OSUB_DONE, "DOWNLOADED");

done:
    pthread_mutex_lock(&g_mu);
    g_busy = 0;
    pthread_mutex_unlock(&g_mu);
    return NULL;
}

void evo_osub_download_start(int index)
{
    pthread_mutex_lock(&g_mu);
    if (g_busy || index < 0 || index >= g_result_count) { pthread_mutex_unlock(&g_mu); return; }
    g_busy = 1;
    ++g_gen;
    unsigned gen = g_gen;
    g_download_index = index;
    g_state = OSUB_WORKING;
    snprintf(g_message, sizeof g_message, "DOWNLOADING...");
    g_file[0] = '\0';
    pthread_mutex_unlock(&g_mu);

    if (!start_worker(download_worker, gen)) {
        pthread_mutex_lock(&g_mu);
        g_busy = 0;
        pthread_mutex_unlock(&g_mu);
        set_state(gen, OSUB_FAILED, "Could not start the download");
    }
}
