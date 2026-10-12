/*
 * provider_stalker.c - Stalker / Ministra portal provider for EVO Player.
 *
 * Speaks the Stalker middleware protocol (the one MAG boxes use) behind the
 * evo_provider_t vtable: handshake -> get_profile -> get_genres ->
 * get_all_channels -> create_link for live TV, and get_categories ->
 * get_ordered_list -> create_link for movies (type=vod) and series
 * (type=series, or episodes inside type=vod items).
 *
 * Several portals can be kept; each is its own folder in the browser, and the
 * categories of each can be hidden and put in another order (provider_stalker.h).
 *
 * The one-line source (adds a portal) is "<portal url> <mac>", for example
 *     http://portal.example.com/c/ 00:1A:79:12:34:56
 * (a space, ';', '|' or ',' all work as the separator). It is persisted to
 * stalker.conf, with /mnt/usb0/.evo_stalker.conf as a fallback to read.
 *
 * The MAC address and the session token are NEVER written to evo.log.
 *
 * UNTESTED ON HARDWARE: written against the protocol, not yet run on a console.
 */
#include "evo_epg.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <time.h>
#include <stdint.h>

#include "evo_provider.h"
#include "evo_net.h"
#include "cJSON.h"
#include "evo_data_path.h"
#include "evo_provider_log.h"
#include "provider_stalker.h"
#include "evo_recent.h"
#include "evo_favorites.h"
#include "evo_title_tag.h"
#include "evo_toast.h"
#include "evo_parental.h"
#include "evo_tmdb.h"
static void parental_mark(const char *provider, evo_provider_item_t *it);

#define STALKER_CONF     "stalker.conf"
#define STALKER_CONF_USB "/mnt/usb0/.evo_stalker.conf"
#define STALKER_CHORD    "stalker_channels.conf"

#define STB_UA "Mozilla/5.0 (QtEmbedded; U; Linux; C) AppleWebKit/533.3 " \
               "(KHTML, like Gecko) MAG200 stbapp ver: 2 rev: 250 Safari/533.3"

#define MAX_CHANNELS 50000

typedef struct {
    char id[24];
    char genre[16];
    char name[96];
    char logo[160];
    char cmd[256];
    char xmltv_id[64];               /* the channel's id in an XMLTV guide, if the portal says */
    int  num;                        /* the channel number the portal gives it */
    int  archive;                    /* tv_archive: the portal keeps recordings */
    int  archive_days;               /* tv_archive_duration (days, or hours) */
} stalker_ch_t;

static char          g_portal[320];     /* as the user gave it, ends in '/' */
static char          g_mac[24];         /* AA:BB:CC:DD:EE:FF */
static char          g_endpoint[400];   /* the load.php / portal.php that answered */
static char          g_token[96];
static int           g_authed;
static stalker_ch_t *g_ch;
static int           g_ch_n;
static char          g_gen_id[512][16]; /* genres, filled by get_genres */
static char          g_gen_title[512][64];
static int           g_gen_n;
static char          g_source[360];

/* ---- several portals, and the user's say over their categories ---------- */
#define MAX_PORTALS 8
#define MAX_PREF    64
#define MAX_ORD     128
enum { SC_LIVE = 0, SC_VOD = 1, SC_SERIES = 2, SC_N = 3 };

typedef struct { char name[40]; char url[320]; char mac[24]; } portal_t;
typedef struct {
    char hid[SC_N][MAX_PREF][16];    /* hidden category ids */
    int  nh[SC_N];
    char ord[SC_N][MAX_ORD][16];     /* the order the user chose */
    int  no[SC_N];
} prefs_t;

static portal_t g_portals[MAX_PORTALS];
static prefs_t  g_prefs[MAX_PORTALS];
static int      g_np;                /* how many portals */
static int      g_active = -1;       /* the one g_portal / g_mac / the session belong to */

/* The visible order last shown per scope: what move up / down works on. */
static char g_vis[SC_N][MAX_ORD][16];
static int  g_vis_n[SC_N];
static int  g_vis_portal = -1;

/* Movie and series categories of this session (live genres are g_gen_*). */
static char g_cat_id[2][256][16];
static char g_cat_title[2][256][64];
static int  g_cat_n[2];

/* How lists are sorted, one setting each for live, movies and series. */
static int g_sort[SC_N];
static const char *const k_live_sort[4] = { "Portal order", "Name A-Z", "Name Z-A", "Channel number" };
static const char *const k_vod_sort[3]  = { "Newest first", "Name A-Z", "Top rated" };
static const char *const k_vod_by[3]    = { "added", "name", "rating" };   /* the portal's sortby */
static int sort_modes(int scope) { return scope == SC_LIVE ? 4 : 3; }

/* The order the user gave the channels of one list ("*" is All Channels) of one portal. */
#define MAX_CHORD    48
#define MAX_CH_ORDER 4000
typedef struct {
    int    portal;
    char   genre[16];
    int    n;
    char (*ids)[24];
} chord_t;
static chord_t g_chord[MAX_CHORD];
static int     g_nchord;

/* The channel list shown last, and the channel being watched (for zapping). */
static char g_last_want[16] = "*";
static int  g_last_portal = -1;
static int  g_play_portal = -1;
static char g_play_id[24];
static char g_play_want[16] = "*";
static char g_play_url[2048];                 /* what is playing, to know it is ours */
static char g_play_ep[EVO_PROVIDER_MAX_ITEM_ID];   /* the series episode playing: "se:200:1:2" */
static int  g_play_ep_portal = -1;
static char g_play_ep_url[2048];

/* ------------------------------------------------------------------------- */
/* Small helpers                                                             */
/* ------------------------------------------------------------------------- */

static void trim(char *s)
{
    if (!s) return;
    size_t n = strlen(s);
    while (n && isspace((unsigned char)s[n - 1])) s[--n] = '\0';
    size_t lead = 0;
    while (s[lead] && isspace((unsigned char)s[lead])) lead++;
    if (lead) memmove(s, s + lead, n - lead + 1);
}

/* "00:1a:79:..", "00-1A-79-..", "001A79..." -> "00:1A:79:..". 0 = ok. */
static int mac_normalize(const char *in, char *out, size_t out_sz)
{
    char hex[13];
    int n = 0;
    if (!in || out_sz < 18) return -1;
    for (; *in; ++in) {
        if (*in == ':' || *in == '-' || *in == '.') continue;
        if (!isxdigit((unsigned char)*in) || n >= 12) return -1;
        hex[n++] = (char)toupper((unsigned char)*in);
    }
    if (n != 12) return -1;
    hex[12] = '\0';
    for (int i = 0; i < 6; ++i) {
        out[i * 3]     = hex[i * 2];
        out[i * 3 + 1] = hex[i * 2 + 1];
        out[i * 3 + 2] = (i < 5) ? ':' : '\0';
    }
    return 0;
}

static void json_str(const cJSON *o, const char *key, char *out, size_t sz)
{
    out[0] = '\0';
    if (!o) return;
    const cJSON *v = cJSON_GetObjectItem((cJSON *)o, key);
    if (!v) return;
    if (cJSON_IsString(v) && v->valuestring)
        snprintf(out, sz, "%s", v->valuestring);
    else if (cJSON_IsNumber(v))
        snprintf(out, sz, "%d", v->valueint);
}

/* The "js" payload, or NULL. Caller owns *root_out and must cJSON_Delete it. */
static cJSON *parse_js(const char *body, cJSON **root_out)
{
    *root_out = NULL;
    if (!body) return NULL;
    cJSON *root = cJSON_Parse(body);
    if (!root) return NULL;
    *root_out = root;
    return cJSON_GetObjectItem(root, "js");
}

/* "ffmpeg http://x", "ffrt http://x", "auto http://x" -> "http://x" */
static const char *strip_cmd(const char *s)
{
    while (*s == ' ') s++;
    if (strncasecmp(s, "http", 4) == 0) return s;
    const char *sp = strchr(s, ' ');
    if (!sp) return s;
    while (*sp == ' ') sp++;
    return sp;
}

/* ------------------------------------------------------------------------- */
/* Config                                                                    */
/* ------------------------------------------------------------------------- */

/* ------------------------------------------------------------------------- */
/* Movie / series cache                                                      */
/* ------------------------------------------------------------------------- */

/*
 * A movie's cmd, or a series item with its seasons and episodes, is only known
 * from the list page that showed it, so the last VC_SLOTS items are kept here.
 * Keys are "<t>:<id>" where t is 'v' (type=vod) or 's' (type=series).
 */
#define VC_SLOTS      256
#define VC_MAX_SEASON 64
#define VC_MAX_EPS    2000

typedef struct {
    char   name[48];
    char   cmd[384];       /* the season's own cmd, "" = use the item's */
    char   sid[32];        /* the portal's id for this season, for fetching its episodes */
    int    fetched;        /* its episodes were asked for already */
    int    n;
    int   *num;            /* episode numbers */
    char **ename;          /* episode titles and cmds, when the portal gives them one by one */
    char **ecmd;
} vseason_t;

typedef struct {
    char       key[48];
    char       cmd[384];
    char       name[64];      /* the title it was listed under */
    char       cat[16];       /* the category it was listed in */
    char       art[EVO_PROVIDER_MAX_ART_URL];   /* its poster, for episodes and Home */
    char       year[8];       /* for Home's big tile */
    char       desc[EVO_PROVIDER_MAX_OVERVIEW];
    int        fetched;       /* its seasons / episodes were asked for already */
    int        nseasons;      /* 0 = a movie */
    int        has_seasons;   /* 0 = one flat list of episodes in seasons[0] */
    vseason_t *seasons;
} vc_t;

static vc_t g_vc[VC_SLOTS];
static int  g_vc_next;

static void vseason_free_eps(vseason_t *sn)
{
    for (int i = 0; i < sn->n; ++i) {
        if (sn->ename) free(sn->ename[i]);
        if (sn->ecmd)  free(sn->ecmd[i]);
    }
    free(sn->ename);
    free(sn->ecmd);
    free(sn->num);
    sn->ename = sn->ecmd = NULL;
    sn->num = NULL;
    sn->n = 0;
}

static void vc_clear(vc_t *e)
{
    for (int i = 0; i < e->nseasons; ++i) vseason_free_eps(&e->seasons[i]);
    free(e->seasons);
    memset(e, 0, sizeof *e);
}

static void vc_free_all(void)
{
    for (int i = 0; i < VC_SLOTS; ++i) vc_clear(&g_vc[i]);
    g_vc_next = 0;
}

static vc_t *vc_find(char t, const char *id)
{
    char key[48];
    snprintf(key, sizeof key, "%c:%s", t, id);
    for (int i = 0; i < VC_SLOTS; ++i)
        if (g_vc[i].key[0] && strcmp(g_vc[i].key, key) == 0) return &g_vc[i];
    return NULL;
}

static int json_int(const cJSON *o, const char *key);

/* The number an episode element stands for. */
static int ep_number(const cJSON *el, int index)
{
    if (cJSON_IsNumber((cJSON *)el)) return el->valueint;
    if (cJSON_IsString((cJSON *)el) && el->valuestring) return atoi(el->valuestring);
    if (cJSON_IsObject((cJSON *)el)) {
        int v = json_int(el, "series_number");
        if (v <= 0) v = json_int(el, "number");
        if (v <= 0) {
            char id[48];
            json_str(el, "id", id, sizeof id);
            const char *c = strrchr(id, ':');
            if (c) v = atoi(c + 1);
        }
        if (v > 0) return v;
    }
    return index + 1;
}

/* Fill one season from a JSON array of episodes (numbers, strings or objects). */
static void vseason_fill(vseason_t *sn, const cJSON *eps)
{
    vseason_free_eps(sn);
    if (!eps || !cJSON_IsArray((cJSON *)eps)) return;
    int n = cJSON_GetArraySize((cJSON *)eps);
    if (n > VC_MAX_EPS) n = VC_MAX_EPS;
    if (n <= 0) return;
    sn->num = (int *)calloc((size_t)n, sizeof(int));
    if (!sn->num) return;
    sn->n = n;
    for (int i = 0; i < n; ++i) {
        cJSON *el = cJSON_GetArrayItem((cJSON *)eps, i);
        sn->num[i] = ep_number(el, i);
        if (!cJSON_IsObject(el)) continue;
        char name[96], cmd[384];
        json_str(el, "name", name, sizeof name);
        evo_title_normalize_spaces(name);
        json_str(el, "cmd", cmd, sizeof cmd);
        if (!name[0] && !cmd[0]) continue;
        if (!sn->ename) sn->ename = (char **)calloc((size_t)n, sizeof(char *));
        if (!sn->ecmd)  sn->ecmd  = (char **)calloc((size_t)n, sizeof(char *));
        if (sn->ename && name[0]) sn->ename[i] = strdup(name);
        if (sn->ecmd  && cmd[0])  sn->ecmd[i]  = strdup(cmd);
    }
}

/* `obj` is the list item; its seasons / episodes are kept when it is a series. */
static void vc_put(char t, const char *id, const char *cmd, const cJSON *obj, int is_series,
                   const char *cat)
{
    vc_t *e = vc_find(t, id);
    if (e && e->fetched && is_series) {
        /* Its seasons and episodes were read from the portal already; the list
         * row knows less. Keep them, take only what the row adds. */
        if (cmd && cmd[0]) snprintf(e->cmd, sizeof e->cmd, "%s", cmd);
        if (cat && cat[0]) snprintf(e->cat, sizeof e->cat, "%s", cat);
        return;
    }
    if (!e) {
        e = &g_vc[g_vc_next];
        g_vc_next = (g_vc_next + 1) % VC_SLOTS;
    }
    vc_clear(e);
    snprintf(e->key, sizeof e->key, "%c:%s", t, id);
    snprintf(e->cmd, sizeof e->cmd, "%s", cmd);
    snprintf(e->cat, sizeof e->cat, "%s", cat ? cat : "");
    if (!is_series || !obj) return;

    cJSON *seasons = cJSON_GetObjectItem((cJSON *)obj, "seasons");
    if (seasons && cJSON_IsArray(seasons) && cJSON_GetArraySize(seasons) > 0) {
        int n = cJSON_GetArraySize(seasons);
        if (n > VC_MAX_SEASON) n = VC_MAX_SEASON;
        e->seasons = (vseason_t *)calloc((size_t)n, sizeof(vseason_t));
        if (!e->seasons) return;
        e->nseasons = n;
        e->has_seasons = 1;
        for (int i = 0; i < n; ++i) {
            cJSON *sn = cJSON_GetArrayItem(seasons, i);
            json_str(sn, "name", e->seasons[i].name, sizeof e->seasons[i].name);
            json_str(sn, "cmd", e->seasons[i].cmd, sizeof e->seasons[i].cmd);
            vseason_fill(&e->seasons[i], cJSON_GetObjectItem(sn, "series"));
        }
    } else {
        e->seasons = (vseason_t *)calloc(1, sizeof(vseason_t));
        if (!e->seasons) return;
        e->nseasons = 1;
        vseason_fill(&e->seasons[0], cJSON_GetObjectItem((cJSON *)obj, "series"));
    }
}

static int json_int(const cJSON *o, const char *key)
{
    char b[24];
    json_str(o, key, b, sizeof b);
    return atoi(b);
}

static void epg_free(void);

static void free_cache(void)
{
    vc_free_all();
    epg_free();
    free(g_ch);
    g_ch = NULL;
    g_ch_n = 0;
    g_gen_n = 0;
    g_cat_n[0] = g_cat_n[1] = 0;
}

static void reset_session(void)
{
    g_authed = 0;
    g_token[0] = '\0';
    g_endpoint[0] = '\0';
    free_cache();
}

static int pref_has(const char (*list)[16], int n, const char *id)
{
    for (int i = 0; i < n; ++i)
        if (strcmp(list[i], id) == 0) return i;
    return -1;
}

static void clean_text(char *s)
{
    for (; *s; ++s)
        if (*s == '|' || (unsigned char)*s < 32) *s = ' ';
}

/* "http://portal.example.com:8080/c/" -> "portal.example.com" */
static void host_name(const char *url, char *out, size_t sz)
{
    const char *h = strstr(url, "://");
    h = h ? h + 3 : url;
    size_t n = 0;
    while (h[n] && h[n] != '/' && h[n] != ':' && n + 1 < sz) { out[n] = h[n]; n++; }
    out[n] = '\0';
    if (!out[0]) snprintf(out, sz, "Portal");
}

static int stalker_save_config(void)
{
    FILE *f = fopen(evo_data_path(STALKER_CONF), "w");
    if (!f) return -1;
    fprintf(f, "# EVO Player - Stalker / Ministra portals\n");
    fprintf(f, "sort=%d|%d|%d\n", g_sort[0], g_sort[1], g_sort[2]);
    for (int i = 0; i < g_np; ++i)
        fprintf(f, "portal=%s|%s|%s\n", g_portals[i].name, g_portals[i].url, g_portals[i].mac);
    for (int i = 0; i < g_np; ++i) {
        const prefs_t *pr = &g_prefs[i];
        for (int sc = 0; sc < SC_N; ++sc) {
            for (int j = 0; j < pr->nh[sc]; ++j)
                fprintf(f, "hide=%d|%d|%s\n", i, sc, pr->hid[sc][j]);
            if (pr->no[sc] > 0) {
                fprintf(f, "order=%d|%d|", i, sc);
                for (int j = 0; j < pr->no[sc]; ++j)
                    fprintf(f, "%s%s", j ? "," : "", pr->ord[sc][j]);
                fprintf(f, "\n");
            }
        }
    }
    fclose(f);
    return 0;
}

static chord_t *chord_find(int portal, const char *genre)
{
    for (int i = 0; i < g_nchord; ++i)
        if (g_chord[i].portal == portal && strcmp(g_chord[i].genre, genre) == 0) return &g_chord[i];
    return NULL;
}

static void chord_free_all(void)
{
    for (int i = 0; i < g_nchord; ++i) free(g_chord[i].ids);
    memset(g_chord, 0, sizeof g_chord);
    g_nchord = 0;
}

/* Replaces the order of one list; an empty list removes it. */
static int chord_set(int portal, const char *genre, char (*ids)[24], int n)
{
    chord_t *c = chord_find(portal, genre);
    if (!c) {
        if (n == 0) return 0;
        if (g_nchord >= MAX_CHORD) return -1;
        c = &g_chord[g_nchord++];
        memset(c, 0, sizeof *c);
        c->portal = portal;
        snprintf(c->genre, sizeof c->genre, "%s", genre);
    }
    free(c->ids);
    c->ids = NULL;
    c->n = 0;
    if (n > 0) {
        c->ids = (char (*)[24])malloc((size_t)n * sizeof *c->ids);
        if (!c->ids) return -1;
        memcpy(c->ids, ids, (size_t)n * sizeof *c->ids);
        c->n = n;
    }
    return 0;
}

static void chord_save(void)
{
    FILE *f = fopen(evo_data_path(STALKER_CHORD), "w");
    if (!f) return;
    for (int i = 0; i < g_nchord; ++i) {
        if (g_chord[i].n <= 0) continue;
        fprintf(f, "%d|%s|", g_chord[i].portal, g_chord[i].genre);
        for (int j = 0; j < g_chord[i].n; ++j)
            fprintf(f, "%s%s", j ? "," : "", g_chord[i].ids[j]);
        fprintf(f, "\n");
    }
    fclose(f);
}

static void chord_load(void)
{
    chord_free_all();
    FILE *f = fopen(evo_data_path(STALKER_CHORD), "r");
    if (!f) return;
    size_t cap = 1 << 16, len = 0;
    char *buf = (char *)malloc(cap);
    int c;
    while (buf && (c = fgetc(f)) != EOF) {
        if (c != '\n') {
            if (len + 2 >= cap) {
                char *nb = (char *)realloc(buf, cap * 2);
                if (!nb) break;
                buf = nb;
                cap *= 2;
            }
            buf[len++] = (char)c;
            continue;
        }
        buf[len] = '\0';
        len = 0;
        char *b1 = strchr(buf, '|');
        char *b2 = b1 ? strchr(b1 + 1, '|') : NULL;
        if (!b2) continue;
        *b1 = *b2 = '\0';
        int portal = atoi(buf);
        char (*ids)[24] = (char (*)[24])malloc((size_t)MAX_CH_ORDER * sizeof *ids);
        if (!ids) continue;
        int n = 0;
        for (char *tok = strtok(b2 + 1, ","); tok && n < MAX_CH_ORDER; tok = strtok(NULL, ","))
            snprintf(ids[n++], 24, "%s", tok);
        chord_set(portal, b1 + 1, ids, n);
        free(ids);
    }
    free(buf);
    fclose(f);
}

/* Makes portal k the one the session belongs to; a different one starts clean. */
static int activate(int k)
{
    if (k < 0 || k >= g_np) return -1;
    if (g_active == k && g_portal[0]) return 0;
    snprintf(g_portal, sizeof g_portal, "%s", g_portals[k].url);
    snprintf(g_mac, sizeof g_mac, "%s", g_portals[k].mac);
    reset_session();
    g_active = k;
    return 0;
}

static void forget_active(void)
{
    g_active = -1;
    g_portal[0] = g_mac[0] = '\0';
    reset_session();
}

static int stalker_init(void)
{
    g_np = 0;
    memset(g_portals, 0, sizeof g_portals);
    memset(g_prefs, 0, sizeof g_prefs);
    memset(g_sort, 0, sizeof g_sort);
    forget_active();
    chord_free_all();

    FILE *f = fopen(evo_data_path(STALKER_CONF), "r");
    if (!f) f = fopen(STALKER_CONF_USB, "r");
    if (!f) return 0;

    char legacy_url[320] = "", legacy_mac[24] = "";
    char line[2048];
    while (fgets(line, sizeof line, f)) {
        trim(line);
        if (line[0] == '#' || line[0] == '\0') continue;
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        char *k = line, *v = eq + 1;
        trim(k);
        trim(v);

        if (strcmp(k, "portal") == 0) {
            char *b1 = strchr(v, '|');
            if (!b1) { snprintf(legacy_url, sizeof legacy_url, "%s", v); continue; }
            char *b2 = strchr(b1 + 1, '|');
            if (!b2 || g_np >= MAX_PORTALS) continue;
            *b1 = *b2 = '\0';
            portal_t *pt = &g_portals[g_np];
            snprintf(pt->name, sizeof pt->name, "%s", v);
            snprintf(pt->url, sizeof pt->url, "%s", b1 + 1);
            if (pt->url[0] && mac_normalize(b2 + 1, pt->mac, sizeof pt->mac) == 0) g_np++;
            else memset(pt, 0, sizeof *pt);
        } else if (strcmp(k, "mac") == 0) {
            mac_normalize(v, legacy_mac, sizeof legacy_mac);
        } else if (strcmp(k, "sort") == 0) {
            int a = 0, b = 0, c = 0;
            if (sscanf(v, "%d|%d|%d", &a, &b, &c) == 3) {
                g_sort[SC_LIVE]   = (a >= 0 && a < 4) ? a : 0;
                g_sort[SC_VOD]    = (b >= 0 && b < 3) ? b : 0;
                g_sort[SC_SERIES] = (c >= 0 && c < 3) ? c : 0;
            }
        } else if (strcmp(k, "hide") == 0 || strcmp(k, "order") == 0) {
            char *b1 = strchr(v, '|');
            char *b2 = b1 ? strchr(b1 + 1, '|') : NULL;
            if (!b2) continue;
            *b1 = *b2 = '\0';
            int i = atoi(v), sc = atoi(b1 + 1);
            char *rest = b2 + 1;
            if (i < 0 || i >= MAX_PORTALS || sc < 0 || sc >= SC_N) continue;
            prefs_t *pr = &g_prefs[i];
            if (k[0] == 'h') {
                if (pr->nh[sc] < MAX_PREF && rest[0])
                    snprintf(pr->hid[sc][pr->nh[sc]++], 16, "%s", rest);
            } else {
                for (char *tok = strtok(rest, ","); tok && pr->no[sc] < MAX_ORD; tok = strtok(NULL, ","))
                    snprintf(pr->ord[sc][pr->no[sc]++], 16, "%s", tok);
            }
        }
    }
    fclose(f);

    /* The single portal of earlier versions: "portal=<url>" and "mac=<mac>". */
    if (g_np == 0 && legacy_url[0] && legacy_mac[0]) {
        portal_t *pt = &g_portals[0];
        host_name(legacy_url, pt->name, sizeof pt->name);
        snprintf(pt->url, sizeof pt->url, "%s", legacy_url);
        snprintf(pt->mac, sizeof pt->mac, "%s", legacy_mac);
        g_np = 1;
    }
    chord_load();
    PROV_LOG("stalker: config loaded (%d portal(s))", g_np);
    return 0;
}

static void stalker_shutdown(void)
{
    free_cache();
}

static int stalker_is_configured(void)
{
    return g_np > 0 ? 1 : 0;
}

/* ------------------------------------------------------------------------- */
/* Request building                                                          */
/* ------------------------------------------------------------------------- */

/*
 * The API lives at <base>portal.php (most portals) or <base>server/load.php
 * (stock Stalker / Ministra under /stalker_portal/). <base> is the portal URL
 * without its trailing "c/". Candidate 0 is tried first, then 1.
 */
static int endpoint_for_url(const char *portal, int idx, char *out, size_t sz)
{
    char base[330];
    snprintf(base, sizeof base, "%s", portal);

    size_t n = strlen(base);
    if (n >= 8 && (strcmp(base + n - 8, "load.php") == 0)) {
        if (idx != 0) return -1;
        snprintf(out, sz, "%s", base);
        return 0;
    }
    if (n >= 10 && (strcmp(base + n - 10, "portal.php") == 0)) {
        if (idx != 0) return -1;
        snprintf(out, sz, "%s", base);
        return 0;
    }
    while (n && base[n - 1] == '/') base[--n] = '\0';
    if (n >= 2 && strcmp(base + n - 2, "/c") == 0) base[n - 1] = '\0';
    else base[n++] = '/', base[n] = '\0';

    if (idx == 0)      snprintf(out, sz, "%sportal.php", base);
    else if (idx == 1) snprintf(out, sz, "%sserver/load.php", base);
    else return -1;
    return 0;
}

static int endpoint_for(int idx, char *out, size_t sz)
{
    return endpoint_for_url(g_portal, idx, out, sz);
}

typedef struct {
    char        cookie[96];
    char        auth[160];
    char        ua[256];
    const char *list[4];
    int         n;
} hdrs_t;

static void make_headers(hdrs_t *h, int with_auth)
{
    h->n = 0;
    snprintf(h->ua, sizeof h->ua, "User-Agent: %s", STB_UA);
    snprintf(h->cookie, sizeof h->cookie,
             "Cookie: mac=%s; stb_lang=en; timezone=Europe/Amsterdam", g_mac);
    h->list[h->n++] = h->ua;
    h->list[h->n++] = h->cookie;
    if (with_auth && g_token[0]) {
        snprintf(h->auth, sizeof h->auth, "Authorization: Bearer %s", g_token);
        h->list[h->n++] = h->auth;
    }
}

static int api_get(const char *query, evo_net_cb cb, void *ctx, int with_auth)
{
    char url[EVO_NET_MAX_URL];
    hdrs_t h;
    int w = snprintf(url, sizeof url, "%s?%s&JsHttpRequest=1-xml", g_endpoint, query);
    if (w < 0 || (size_t)w >= sizeof url) return -13;
    make_headers(&h, with_auth);
    return evo_net_request_async("GET", url, NULL, h.list, h.n, cb, ctx);
}


/* ------------------------------------------------------------------------- */
/* Lists kept on disk (24 h)                                                 */
/* ------------------------------------------------------------------------- */
/*
 * Categories, list pages and a series' seasons change rarely, but every visit
 * asked the portal again. They are kept under <data>/stalker_cache, one file
 * per request, per portal and MAC, and answered from there for 24 hours; after
 * that the portal is asked and the file replaced. If the portal does not
 * answer, an older copy is used rather than an empty screen. Stream links
 * (create_link), search and live channels are never kept: they carry tokens or
 * change by the minute.
 */
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>

#define SC_TTL_SEC   (24 * 60 * 60)
#define SC_PRUNE_SEC (7 * 24 * 60 * 60)
#define SC_MAX_BYTES (4 * 1024 * 1024)

static unsigned long long sc_fnv(const char *s, unsigned long long h)
{
    for (; s && *s; ++s) h = (h ^ (unsigned char)*s) * 1099511628211ull;
    return h;
}

static int sc_dir(char *out, size_t sz)
{
    snprintf(out, sz, "%s", evo_data_path("stalker_cache"));
    struct stat st;
    if (stat(out, &st) == 0) return 0;
    return mkdir(out, 0777) == 0 ? 0 : -1;
}

static int sc_path(const char *query, char *out, size_t sz)
{
    char dir[512];
    if (sc_dir(dir, sizeof dir) != 0) return -1;
    unsigned long long h = sc_fnv(g_portal, 1469598103934665603ull);
    h = sc_fnv("|", h);
    h = sc_fnv(g_mac, h);
    h = sc_fnv("|", h);
    h = sc_fnv(query, h);
    int w = snprintf(out, sz, "%s/%016llx.json", dir, h);
    return (w > 0 && (size_t)w < sz) ? 0 : -1;
}

/* Files nobody asked for in a week go, once per session. */
static void sc_prune(void)
{
    static int done;
    if (done) return;
    done = 1;
    char dir[512];
    if (sc_dir(dir, sizeof dir) != 0) return;
    DIR *d = opendir(dir);
    if (!d) return;
    const time_t now = time(NULL);
    struct dirent *de;
    int removed = 0;
    while ((de = readdir(d)) != NULL) {
        if (de->d_name[0] == '.') continue;
        char path[800];
        snprintf(path, sizeof path, "%s/%s", dir, de->d_name);
        struct stat st;
        if (stat(path, &st) == 0 && now - st.st_mtime > SC_PRUNE_SEC && unlink(path) == 0)
            removed++;
    }
    closedir(d);
    if (removed) PROV_LOG("stalker: cache: %d old file(s) removed", removed);
}

/* The body, NUL-terminated, or NULL. *age_out is seconds since it was saved. */
static char *sc_read(const char *path, size_t *len_out, long *age_out)
{
    struct stat st;
    if (stat(path, &st) != 0 || st.st_size <= 0 || st.st_size > SC_MAX_BYTES) return NULL;
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    char *buf = (char *)malloc((size_t)st.st_size + 1);
    size_t n = buf ? fread(buf, 1, (size_t)st.st_size, f) : 0;
    fclose(f);
    if (!buf || n != (size_t)st.st_size) { free(buf); return NULL; }
    buf[n] = '\0';
    *len_out = n;
    *age_out = (long)(time(NULL) - st.st_mtime);
    return buf;
}

/* A real answer, not a refusal or an error page, is worth keeping. */
static int sc_worth_keeping(const char *body, size_t len)
{
    return body && len > 16 && len <= SC_MAX_BYTES && strstr(body, "\"js\"") &&
           !strstr(body, "Authorization failed") && !strstr(body, "\"js\":[]") &&
           !strstr(body, "\"js\":null") && !strstr(body, "\"js\":false");
}

static void sc_write(const char *path, const char *body, size_t len)
{
    char tmp[820];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    if (!f) return;
    const int ok = fwrite(body, 1, len, f) == len;
    fclose(f);
    if (!ok || rename(tmp, path) != 0) unlink(tmp);
}

typedef struct {
    evo_net_cb cb;
    void      *ctx;
    char       path[800];
} sc_wait_t;

static void on_sc_net(int success, int status, const char *body, size_t len, void *ud)
{
    sc_wait_t *w = (sc_wait_t *)ud;
    const int good = success && status >= 200 && status < 300;
    if (good && sc_worth_keeping(body, len)) {
        sc_write(w->path, body, len);
        w->cb(success, status, body, len, w->ctx);
    } else {
        size_t n = 0;
        long age = 0;
        char *old = sc_read(w->path, &n, &age);
        if (old) {
            PROV_LOG("stalker: cache: portal did not answer (http=%d), using a copy %ld h old",
                     status, age / 3600);
            w->cb(1, 200, old, n, w->ctx);
            free(old);
        } else {
            w->cb(success, status, body, len, w->ctx);
        }
    }
    free(w);
}

/* api_get for a list: from disk while it is under a day old, else the portal. */
static int api_get_cached(const char *query, evo_net_cb cb, void *ctx)
{
    sc_prune();
    sc_wait_t *w = (sc_wait_t *)calloc(1, sizeof *w);
    if (!w || sc_path(query, w->path, sizeof w->path) != 0) {
        free(w);
        return api_get(query, cb, ctx, 1);
    }
    size_t n = 0;
    long age = 0;
    char *body = sc_read(w->path, &n, &age);
    if (body && age >= 0 && age < SC_TTL_SEC && sc_worth_keeping(body, n)) {
        free(w);
        cb(1, 200, body, n, ctx);         /* the same path a portal answer takes */
        free(body);
        return 0;
    }
    free(body);
    w->cb = cb;
    w->ctx = ctx;
    int rc = api_get(query, on_sc_net, w, 1);
    if (rc != 0) free(w);
    return rc;
}

/* Forget every kept list (a portal was added, removed or changed). */
static void sc_clear_all(void)
{
    char dir[512];
    if (sc_dir(dir, sizeof dir) != 0) return;
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (de->d_name[0] == '.') continue;
        char path[800];
        snprintf(path, sizeof path, "%s/%s", dir, de->d_name);
        unlink(path);
    }
    closedir(d);
}


/* ------------------------------------------------------------------------- */
/* Recently watched channels, and when the subscription ends                 */
/* ------------------------------------------------------------------------- */
/*
 * Live TV gets a "Recently watched" category at the top: the last channels that
 * actually started, newest first, per portal, kept across launches. And each
 * portal remembers the end date of its subscription, shown under its name.
 */
#define RCH_MAX 12
static char g_rch[MAX_PORTALS][RCH_MAX][24];
static int  g_rch_n[MAX_PORTALS];
static char g_expiry[MAX_PORTALS][24];          /* "2026-12-01", "" unknown */
static int  g_state_loaded;

static void state_save(void)
{
    FILE *f = fopen(evo_data_path("stalker_state.txt"), "w");
    if (!f) return;
    for (int k = 0; k < g_np && k < MAX_PORTALS; ++k) {
        if (g_expiry[k][0]) fprintf(f, "end|%d|%s\n", k, g_expiry[k]);
        for (int i = 0; i < g_rch_n[k]; ++i) fprintf(f, "ch|%d|%s\n", k, g_rch[k][i]);
    }
    fclose(f);
}

static void state_load(void)
{
    if (g_state_loaded) return;
    g_state_loaded = 1;
    FILE *f = fopen(evo_data_path("stalker_state.txt"), "r");
    if (!f) return;
    char line[128];
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = '\0';
        char *a = strchr(line, '|'), *b = a ? strchr(a + 1, '|') : NULL;
        if (!a || !b) continue;
        *a = *b = '\0';
        const int k = atoi(a + 1);
        if (k < 0 || k >= MAX_PORTALS) continue;
        if (strcmp(line, "end") == 0) snprintf(g_expiry[k], sizeof g_expiry[k], "%s", b + 1);
        else if (strcmp(line, "ch") == 0 && g_rch_n[k] < RCH_MAX)
            snprintf(g_rch[k][g_rch_n[k]++], sizeof g_rch[0][0], "%s", b + 1);
    }
    fclose(f);
}

static void rch_push(int k, const char *id)
{
    if (k < 0 || k >= MAX_PORTALS || !id || !id[0]) return;
    state_load();
    char keep[RCH_MAX][24];
    int n = 0;
    snprintf(keep[n++], sizeof keep[0], "%s", id);
    for (int i = 0; i < g_rch_n[k] && n < RCH_MAX; ++i)
        if (strcmp(g_rch[k][i], id) != 0) snprintf(keep[n++], sizeof keep[0], "%s", g_rch[k][i]);
    memcpy(g_rch[k], keep, sizeof keep);
    g_rch_n[k] = n;
    state_save();
}

/* The portal list moved up one after removing portal `removed`. */
static void state_renumber(int removed)
{
    state_load();
    for (int k = removed; k < MAX_PORTALS - 1; ++k) {
        memcpy(g_rch[k], g_rch[k + 1], sizeof g_rch[k]);
        g_rch_n[k] = g_rch_n[k + 1];
        memcpy(g_expiry[k], g_expiry[k + 1], sizeof g_expiry[k]);
    }
    g_rch_n[MAX_PORTALS - 1] = 0;
    g_expiry[MAX_PORTALS - 1][0] = '\0';
    state_save();
}

/* Days from 1970-01-01 to y-m-d (proleptic Gregorian). */
static long days_from_civil(int y, int m, int d)
{
    y -= m <= 2;
    const long era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153u * (unsigned)(m + (m > 2 ? -3 : 9)) + 2u) / 5u + (unsigned)d - 1u;
    const unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
    return era * 146097L + (long)doe - 719468L;
}

/* "2026-12-01 00:00:00", "01.12.2026", or seconds since 1970 -> "2026-12-01". */
static int expiry_normalize(const char *in, char *out, size_t sz)
{
    int y = 0, m = 0, d = 0;
    if (!in || !in[0]) return 0;
    if (sscanf(in, "%4d-%2d-%2d", &y, &m, &d) == 3 ||
        (sscanf(in, "%2d.%2d.%4d", &d, &m, &y) == 3) ||
        (sscanf(in, "%2d/%2d/%4d", &d, &m, &y) == 3)) {
        if (y < 2000 || m < 1 || m > 12 || d < 1 || d > 31) return 0;
    } else {
        long long t = atoll(in);
        if (t < 946684800LL) return 0;                /* not a date */
        long days = (long)(t / 86400);
        /* civil from days */
        long z = days + 719468L;
        const long era = (z >= 0 ? z : z - 146096) / 146097;
        const unsigned doe = (unsigned)(z - era * 146097);
        const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
        const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
        const unsigned mp = (5 * doy + 2) / 153;
        d = (int)(doy - (153 * mp + 2) / 5 + 1);
        m = (int)(mp < 10 ? mp + 3 : mp - 9);
        y = (int)(yoe + era * 400) + (m <= 2);
    }
    snprintf(out, sz, "%04d-%02d-%02d", y, m, d);
    return 1;
}

/* Look in an answer for the end date the portal gives (field names differ). */
static void expiry_take(int k, cJSON *js)
{
    static const char *keys[] = { "end_date", "expire_billing_date", "tariff_expired_date",
                                  "expiry", "exp_date", "expire_date", NULL };
    if (!js || k < 0 || k >= MAX_PORTALS) return;
    char v[48] = "", d[24];
    for (int i = 0; keys[i] && !v[0]; ++i) json_str(js, keys[i], v, sizeof v);
    if (!v[0] || !expiry_normalize(v, d, sizeof d)) return;
    state_load();
    if (strcmp(g_expiry[k], d) != 0) {
        snprintf(g_expiry[k], sizeof g_expiry[k], "%s", d);
        state_save();
    }
    PROV_LOG("stalker: subscription of portal %d ends %s", k, d);
    int y, m, dd;
    if (sscanf(d, "%d-%d-%d", &y, &m, &dd) == 3) {
        const long left = days_from_civil(y, m, dd) - (long)(time(NULL) / 86400);
        if (left >= 0 && left <= 7) {
            char msg[96];
            snprintf(msg, sizeof msg, "Subscription ends in %ld day%s (%s)", left, left == 1 ? "" : "s", d);
            toast("STALKER / MINISTRA", msg);
        }
    }
}

static void on_main_info(int success, int status, const char *body, size_t len, void *ud)
{
    (void)len;
    const int k = (int)(intptr_t)ud;
    if (!success || status < 200 || status >= 300) return;
    cJSON *root = NULL;
    cJSON *js = parse_js(body, &root);
    if (js && cJSON_IsObject(js)) expiry_take(k, js);
    if (root) cJSON_Delete(root);
}

/* ------------------------------------------------------------------------- */
/* Authentication: handshake, then get_profile                               */
/* ------------------------------------------------------------------------- */

typedef struct {
    evo_provider_auth_cb cb;
    void *ud;
    int   ep_idx;
} auth_ctx_t;

static void auth_fail(auth_ctx_t *c, const char *msg)
{
    g_authed = 0;
    g_token[0] = '\0';
    if (c->cb) c->cb(0, msg, c->ud);
    free(c);
}

static int handshake_start(auth_ctx_t *c);
static void preload_channels(void);

static void on_profile(int success, int status, const char *body, size_t len, void *ud)
{
    (void)len; (void)body;
    auth_ctx_t *c = (auth_ctx_t *)ud;
    if (!success || status < 200 || status >= 300) {
        PROV_LOG("stalker: get_profile failed: ok=%d http=%d", success, status);
        auth_fail(c, "Portal rejected the profile - is the MAC address activated?");
        return;
    }
    g_authed = 1;
    PROV_LOG("stalker: signed in");
    {
        const int before = (g_active >= 0 && g_active < MAX_PORTALS) ? (state_load(), g_expiry[g_active][0]) : 1;
        cJSON *root = NULL;
        cJSON *js = parse_js(body, &root);
        if (js && cJSON_IsObject(js)) expiry_take(g_active, js);
        if (root) cJSON_Delete(root);
        (void)before;
        if (g_active >= 0 && g_active < MAX_PORTALS)
            api_get("type=account_info&action=get_main_info", on_main_info,
                    (void *)(intptr_t)g_active, 1);
    }
    if (c->cb) c->cb(1, "Portal connected", c->ud);
    free(c);
    /* Live TV and the first search should not wait for the channel list:
     * read the kept one, or fetch it now, behind the scenes. */
    preload_channels();
}

static void on_handshake(int success, int status, const char *body, size_t len, void *ud)
{
    (void)len;
    auth_ctx_t *c = (auth_ctx_t *)ud;

    char token[sizeof g_token] = {0};
    if (success && status >= 200 && status < 300) {
        cJSON *root;
        cJSON *js = parse_js(body, &root);
        if (js) json_str(js, "token", token, sizeof token);
        if (root) cJSON_Delete(root);
    }

    if (!token[0]) {
        /* This endpoint did not answer like a portal: try the next one. */
        PROV_LOG("stalker: handshake try %d: ok=%d http=%d, no token", c->ep_idx, success, status);
        c->ep_idx++;
        if (handshake_start(c) != 0) auth_fail(c, "Portal did not answer");
        return;
    }

    snprintf(g_token, sizeof g_token, "%s", token);
    int rc = api_get("type=stb&action=get_profile&hd=1&num_banks=2&stb_type=MAG250"
                     "&client_type=STB&image_version=218&video_out=hdmi"
                     "&auth_second_step=0&hw_version=1.7-BD-00&not_valid_token=0"
                     "&api_signature=262",
                     on_profile, c, 1);
    if (rc != 0) auth_fail(c, "Could not reach the portal");
}

/* 0 = request queued (c is now owned by the callback), nonzero = no more tries. */
static int handshake_start(auth_ctx_t *c)
{
    if (endpoint_for(c->ep_idx, g_endpoint, sizeof g_endpoint) != 0) return -1;
    g_token[0] = '\0';
    return api_get("type=stb&action=handshake&token=&prehash=0", on_handshake, c, 0) == 0
               ? 0 : -2;
}

static int stalker_auth(evo_provider_auth_cb cb, void *ud)
{
    if (g_np > 0 && !g_portal[0]) activate(0);
    if (!g_portal[0] || !g_mac[0]) {
        if (cb) cb(0, "Enter the portal URL and MAC address first", ud);
        return -1;
    }
    reset_session();
    PROV_LOG("stalker: signing in");
    auth_ctx_t *c = (auth_ctx_t *)calloc(1, sizeof *c);
    if (!c) return -2;
    c->cb = cb;
    c->ud = ud;
    if (handshake_start(c) != 0) {
        free(c);
        if (cb) cb(0, "Invalid portal URL", ud);
        return -3;
    }
    return 0;
}

/* ------------------------------------------------------------------------- */
/* Operations that need a session: catalog and resolve                       */
/* ------------------------------------------------------------------------- */

enum { OP_CATALOG = 1, OP_RESOLVE = 2, OP_SEARCH = 3, OP_PRELOAD = 4 };
static int g_ch_loading;             /* the channel list is on its way (search loads it behind the scenes) */

typedef struct {
    int  op;
    char arg[EVO_PROVIDER_MAX_ITEM_ID];  /* parent_id or item_id */
    int  page;
    char cmd[384];                       /* resolve: the item's cmd */
    char type;                           /* movies/series: 'v' vod, 's' series */
    int  live;                           /* resolve: a live channel */
    char ltype[8];                       /* resolve: the type create_link was asked with */
    char lser[16];                       /* resolve: ... and its series number */
    int  alt_tried;                      /* resolve: the other type was tried already */
    int  refetched;                      /* resolve: the series was asked for again (Recent) */
    int  searched;                       /* resolve: the film was looked up by its name */
    int  cached_cmd;                     /* resolve: the cmd came from memory / the kept list */
    int  same_n;                         /* resolve by name: rows with the same name so far */
    char same_cmd[384];                  /* ... and the first one's cmd */
    evo_provider_item_t *acc;            /* search: results so far */
    int  acc_n;
    int  stage;                          /* search: 1 movies, 2 series, 3 done */
    int  only;                           /* search: just this stage (Home's TMDB rows) */
    evo_provider_items_cb   icb;
    evo_provider_resolve_cb rcb;
    void *ud;
} op_t;

static void op_fail(op_t *p)
{
    if (p->op == OP_RESOLVE) { if (p->rcb) p->rcb(0, NULL, 0, p->ud); }
    else                     { if (p->icb) p->icb(0, NULL, 0, 0, p->ud); }
    free(p->acc);
    free(p);
}

static void op_run(op_t *p);
static op_t *op_new(int op, const char *arg, int page);

/*
 * One sign-in at a time. Signing in starts a fresh session (and drops the
 * cached lists), so several requests that each signed in for themselves -
 * Home's newest films and series and a TMDB lookup at once - kept throwing
 * away each other's session. The others wait and go once it is done.
 */
#define AUTH_WAIT_MAX 64
static op_t *g_auth_wait[AUTH_WAIT_MAX];
static int   g_auth_wait_n;
static int   g_auth_busy;

static void op_run(op_t *p);

static void on_op_auth(int ok, const char *msg, void *ud)
{
    (void)msg;
    op_t *p = (op_t *)ud;
    g_auth_busy = 0;
    op_t *waiting[AUTH_WAIT_MAX];
    const int n = g_auth_wait_n;
    memcpy(waiting, g_auth_wait, sizeof waiting[0] * (size_t)n);
    g_auth_wait_n = 0;
    if (!ok) {
        op_fail(p);
        for (int i = 0; i < n; ++i) op_fail(waiting[i]);
        return;
    }
    op_run(p);
    for (int i = 0; i < n; ++i) op_run(waiting[i]);
}

/* ---- catalog ----------------------------------------------------------- */

/* Remember what is on screen, so a move up / down knows the neighbours. */
static void note_visible(int scope, const char (*ids)[16], const int *idx, int k)
{
    g_vis_portal = g_active;
    g_vis_n[scope] = 0;
    for (int i = 0; i < k && g_vis_n[scope] < MAX_ORD; ++i)
        snprintf(g_vis[scope][g_vis_n[scope]++], 16, "%s", ids[idx[i]]);
}

/* ids[0..n) in the user's order with the hidden ones left out; returns how many. */
static int apply_prefs(int scope, const char (*ids)[16], int n, int *out)
{
    int k = 0;
    if (n > 512) n = 512;
    char used[512];
    memset(used, 0, sizeof used);
    const prefs_t *pr = (g_active >= 0 && g_active < g_np) ? &g_prefs[g_active] : NULL;

    if (pr)
        for (int j = 0; j < pr->no[scope]; ++j)
            for (int i = 0; i < n; ++i)
                if (!used[i] && strcmp(ids[i], pr->ord[scope][j]) == 0 &&
                    pref_has(pr->hid[scope], pr->nh[scope], ids[i]) < 0) {
                    out[k++] = i;
                    used[i] = 1;
                    break;
                }
    for (int i = 0; i < n; ++i)
        if (!used[i] && !(pr && pref_has(pr->hid[scope], pr->nh[scope], ids[i]) >= 0))
            out[k++] = i;
    note_visible(scope, ids, out, k);
    return k;
}

static void emit_genres(op_t *p)
{
    int idx[512];
    int k = apply_prefs(SC_LIVE, (const char (*)[16])g_gen_id, g_gen_n, idx);
    state_load();
    const int recent = (g_active >= 0 && g_active < MAX_PORTALS && g_rch_n[g_active] > 0) ? 1 : 0;
    int narch = 0;
    for (int i = 0; i < g_ch_n; ++i) narch += g_ch[i].archive;
    const int archive = (g_ch_n == 0 || narch > 0) ? 1 : 0;   /* not loaded yet: offer it */
    int n = k + 1 + recent + archive;
    evo_provider_item_t *items = (evo_provider_item_t *)calloc((size_t)n, sizeof *items);
    if (!items) { op_fail(p); return; }

    for (int i = 0; i < n; ++i) {
        evo_provider_item_clear(&items[i]);
        items[i].kind = EVO_MEDIA_FOLDER;
        items[i].is_folder = 1;
        if (recent && i == 0) {
            snprintf(items[i].id, sizeof items[i].id, "g:@recent");
            snprintf(items[i].title, sizeof items[i].title, "Recently Watched");
            snprintf(items[i].subtitle, sizeof items[i].subtitle, "%d channels", g_rch_n[g_active]);
        } else if (i == recent) {
            snprintf(items[i].id, sizeof items[i].id, "g:*");
            snprintf(items[i].title, sizeof items[i].title, "All Channels");
            snprintf(items[i].subtitle, sizeof items[i].subtitle, "%d channels", g_ch_n);
        } else if (archive && i == recent + 1) {
            snprintf(items[i].id, sizeof items[i].id, "g:@archive");
            snprintf(items[i].title, sizeof items[i].title, "Catch-up");
            if (g_ch_n) snprintf(items[i].subtitle, sizeof items[i].subtitle, "%d channels with an archive", narch);
            else        snprintf(items[i].subtitle, sizeof items[i].subtitle, "Programmes already broadcast");
        } else {
            int j = idx[i - 1 - recent - archive];
            snprintf(items[i].id, sizeof items[i].id, "g:%s", g_gen_id[j]);
            snprintf(items[i].title, sizeof items[i].title, "%s", g_gen_title[j]);
            parental_mark("stalker", &items[i]);
            snprintf(items[i].subtitle, sizeof items[i].subtitle, "Live TV category");
        }
    }
    if (p->icb) p->icb(1, items, n, 0, p->ud);
    free(items);
    free(p);
}

static int cmp_name(const stalker_ch_t *a, const stalker_ch_t *b)
{
    return strcasecmp(a->name, b->name);
}

static int cmp_az(const void *x, const void *y)
{
    return cmp_name(&g_ch[*(const int *)x], &g_ch[*(const int *)y]);
}

static int cmp_za(const void *x, const void *y)
{
    return cmp_name(&g_ch[*(const int *)y], &g_ch[*(const int *)x]);
}

/* By the portal's channel number; channels without one go last, ties by name. */
static int cmp_num(const void *x, const void *y)
{
    const stalker_ch_t *a = &g_ch[*(const int *)x], *b = &g_ch[*(const int *)y];
    int na = a->num > 0 ? a->num : 1 << 30, nb = b->num > 0 ? b->num : 1 << 30;
    if (na != nb) return na < nb ? -1 : 1;
    return cmp_name(a, b);
}

/*
 * The channels of one category ("*" = all) in the order they are shown: the sort
 * chosen, or with "Portal order" the user's own moves first. idx gets g_ch_n room.
 */
static int channel_order(const char *want, int *idx)
{
    if (strcmp(want, "@archive") == 0) {              /* channels that keep recordings */
        int m = 0;
        for (int i = 0; i < g_ch_n; ++i)
            if (g_ch[i].archive) idx[m++] = i;
        return m;
    }
    if (strcmp(want, "@recent") == 0) {               /* newest first, never re-sorted */
        state_load();
        int m = 0;
        if (g_active >= 0 && g_active < MAX_PORTALS)
            for (int r = 0; r < g_rch_n[g_active]; ++r)
                for (int i = 0; i < g_ch_n; ++i)
                    if (strcmp(g_ch[i].id, g_rch[g_active][r]) == 0) { idx[m++] = i; break; }
        return m;
    }
    int all = (strcmp(want, "*") == 0);
    int m = 0;
    for (int i = 0; i < g_ch_n; ++i)
        if (all || strcmp(g_ch[i].genre, want) == 0) idx[m++] = i;

    switch (g_sort[SC_LIVE]) {
    case 1:  qsort(idx, (size_t)m, sizeof *idx, cmp_az);  break;
    case 2:  qsort(idx, (size_t)m, sizeof *idx, cmp_za);  break;
    case 3:  qsort(idx, (size_t)m, sizeof *idx, cmp_num); break;
    default: {                                        /* the portal's own order, then the user's */
        const chord_t *co = chord_find(g_active, want);
        if (!co || co->n <= 0 || m == 0) break;
        int *out = (int *)malloc((size_t)m * sizeof *out);
        char *used = (char *)calloc((size_t)m, 1);
        if (!out || !used) { free(out); free(used); break; }
        int k = 0;
        for (int j = 0; j < co->n; ++j)
            for (int i = 0; i < m; ++i)
                if (!used[i] && strcmp(g_ch[idx[i]].id, co->ids[j]) == 0) {
                    out[k++] = idx[i];
                    used[i] = 1;
                    break;
                }
        for (int i = 0; i < m; ++i)
            if (!used[i]) out[k++] = idx[i];
        memcpy(idx, out, (size_t)m * sizeof *idx);
        free(out);
        free(used);
        break;
    }
    }
    return m;
}


/* ------------------------------------------------------------------------- */
/* TV guide: now and next for every channel                                  */
/* ------------------------------------------------------------------------- */
/*
 * One get_epg_info request covers every channel for the next two hours. It is
 * asked for when a channel list is first shown and again after half an hour;
 * the list shows "now" and "next" from it, and ProviderHostScreen redraws the
 * list when a new guide arrives (stalker_epg_generation). A portal without a
 * guide simply answers with nothing, and the rows stay as they were.
 */
#define EPG_PROGS      3
#define EPG_REFRESH_S  (30 * 60)
#define EPG_RETRY_S    (10 * 60)

typedef struct {
    char name[96];
    long start, stop;
} epg_prog_t;

typedef struct {
    char       ch[24];
    int        n;
    epg_prog_t prog[EPG_PROGS];
} epg_ch_t;

static epg_ch_t *g_epg;
static int       g_epg_n;
static time_t    g_epg_at;           /* when it was last asked for */
static int       g_epg_busy;
static int       g_epg_portal = -1;  /* the portal the guide belongs to */
static unsigned  g_epg_gen;

static void epg_free(void)
{
    free(g_epg);
    g_epg = NULL;
    g_epg_n = 0;
    g_epg_at = 0;
    g_epg_portal = -1;
}

static long json_long(const cJSON *o, const char *key)
{
    char b[24];
    json_str(o, key, b, sizeof b);
    return atol(b);
}

static int epg_cmp(const void *a, const void *b)
{
    return strcmp(((const epg_ch_t *)a)->ch, ((const epg_ch_t *)b)->ch);
}

static void on_epg(int success, int status, const char *body, size_t len, void *ud)
{
    (void)len;
    const int portal = (int)(intptr_t)ud;
    g_epg_busy = 0;
    if (portal != g_active) return;                   /* another portal by now */

    cJSON *root = NULL;
    cJSON *js = (success && status >= 200 && status < 300) ? parse_js(body, &root) : NULL;
    cJSON *data = js && cJSON_IsObject(js) ? cJSON_GetObjectItem(js, "data") : NULL;
    if (!data || !cJSON_IsObject(data)) {
        PROV_LOG("stalker: no TV guide from this portal (http=%d)", status);
        if (root) cJSON_Delete(root);
        g_epg_at = time(NULL) - EPG_REFRESH_S + EPG_RETRY_S;   /* try again in 10 min */
        return;
    }

    int n = 0;
    for (cJSON *c = data->child; c; c = c->next) n++;
    epg_ch_t *all = (epg_ch_t *)calloc((size_t)(n ? n : 1), sizeof *all);
    if (!all) { cJSON_Delete(root); return; }

    int k = 0;
    for (cJSON *c = data->child; c && k < n; c = c->next) {
        if (!c->string || !cJSON_IsArray(c)) continue;
        epg_ch_t *e = &all[k];
        snprintf(e->ch, sizeof e->ch, "%s", c->string);
        const time_t now = time(NULL);
        int m = cJSON_GetArraySize(c);
        for (int i = 0; i < m && e->n < EPG_PROGS; ++i) {
            cJSON *pr = cJSON_GetArrayItem(c, i);
            long a = json_long(pr, "start_timestamp"), b = json_long(pr, "stop_timestamp");
            if (b <= 0 || b <= (long)now) continue;   /* already over */
            epg_prog_t *g = &e->prog[e->n];
            json_str(pr, "name", g->name, sizeof g->name);
            if (!g->name[0]) continue;
            g->start = a;
            g->stop = b;
            e->n++;
        }
        if (e->n > 0) k++;
    }
    cJSON_Delete(root);
    qsort(all, (size_t)k, sizeof *all, epg_cmp);

    free(g_epg);
    g_epg = all;
    g_epg_n = k;
    g_epg_portal = portal;
    g_epg_gen++;
    PROV_LOG("stalker: TV guide for %d channel(s)", k);
}

/* Ask for the guide when there is none, or it is half an hour old. */
static void epg_refresh(void)
{
    const time_t now = time(NULL);
    if (g_epg_busy || !g_authed || g_active < 0) return;
    if (g_epg_portal != g_active) epg_free();
    if (g_epg_at && now - g_epg_at < EPG_REFRESH_S) return;
    g_epg_at = now;
    g_epg_busy = 1;
    if (api_get("type=itv&action=get_epg_info&period=2", on_epg,
                (void *)(intptr_t)g_active, 1) != 0)
        g_epg_busy = 0;
}

/* "now" and "next" for one channel, from the guide in memory. */
static void epg_fill(evo_provider_item_t *it, const char *ch_id)
{
    if (!g_epg_n || g_epg_portal != g_active) return;
    epg_ch_t key;
    snprintf(key.ch, sizeof key.ch, "%s", ch_id);
    /* A binary search by hand: bsearch is not used anywhere else, and a libc
     * function the console leaves out crashes when called (as strcasestr did). */
    const epg_ch_t *e = NULL;
    for (int lo = 0, hi = g_epg_n - 1; lo <= hi && !e; ) {
        const int mid = lo + (hi - lo) / 2;
        const int c = epg_cmp(&key, &g_epg[mid]);
        if (c == 0) e = &g_epg[mid];
        else if (c < 0) hi = mid - 1;
        else lo = mid + 1;
    }
    if (!e) return;
    const long now = (long)time(NULL);
    int at = -1;
    for (int i = 0; i < e->n; ++i)
        if (e->prog[i].start <= now && now < e->prog[i].stop) { at = i; break; }
    if (at < 0) {                                     /* between programmes: the next one */
        for (int i = 0; i < e->n; ++i)
            if (e->prog[i].start > now) {
                snprintf(it->next_title, sizeof it->next_title, "%s", e->prog[i].name);
                return;
            }
        return;
    }
    snprintf(it->now_title, sizeof it->now_title, "%s", e->prog[at].name);
    if (at + 1 < e->n)
        snprintf(it->next_title, sizeof it->next_title, "%s", e->prog[at + 1].name);
}

unsigned stalker_epg_generation(void)
{
    return g_epg_gen;
}

static int channel_archive_days(const stalker_ch_t *c);

static void emit_channels(op_t *p)
{
    const char *want = p->arg + 2;           /* after "g:" */
    int skip = p->page * EVO_PROVIDER_PAGE_MAX;
    int taken = 0, has_more = 0;

    evo_provider_item_t *items =
        (evo_provider_item_t *)calloc(EVO_PROVIDER_PAGE_MAX, sizeof *items);
    int *idx = (int *)malloc((size_t)(g_ch_n ? g_ch_n : 1) * sizeof *idx);
    if (!items || !idx) { free(items); free(idx); op_fail(p); return; }

    const int catchup = strcmp(want, "@archive") == 0;
    if (!catchup) {                                   /* zapping stays in a live list */
        snprintf(g_last_want, sizeof g_last_want, "%s", want);
        g_last_portal = g_active;
    }
    int m = channel_order(want, idx);

    for (int j = 0; j < m; ++j) {
        const stalker_ch_t *c = &g_ch[idx[j]];
        if (skip > 0) { skip--; continue; }
        if (taken >= EVO_PROVIDER_PAGE_MAX) { has_more = 1; break; }

        evo_provider_item_t *it = &items[taken++];
        evo_provider_item_clear(it);
        snprintf(it->parent_id, sizeof it->parent_id, "%s", p->arg);
        snprintf(it->title, sizeof it->title, "%s", c->name[0] ? c->name : "Channel");
        if (strncmp(c->logo, "http", 4) == 0)
            snprintf(it->art_url, sizeof it->art_url, "%s", c->logo);
        if (catchup) {
            snprintf(it->id, sizeof it->id, "ar:%s", c->id);
            snprintf(it->subtitle, sizeof it->subtitle, "Catch-up - %d days", channel_archive_days(c));
            it->kind = EVO_MEDIA_FOLDER;
            it->is_folder = 1;
            continue;
        }
        snprintf(it->id, sizeof it->id, "ch:%s", c->id);
        snprintf(it->subtitle, sizeof it->subtitle, "Live Broadcast");
        it->kind = EVO_MEDIA_STREAM;
        it->is_live = 1;
        it->is_folder = 0;
        epg_fill(it, c->id);
        /* The portal's guide first; the TV guide from Settings fills the gaps. */
        evo_epg_fill_item(it, NULL, c->xmltv_id[0] ? c->xmltv_id : NULL, c->name);
    }
    epg_refresh();                    /* the next list shows a newer guide */
    if (p->icb) p->icb(1, items, taken, has_more, p->ud);
    free(idx);
    free(items);
    free(p);
}

static void on_genres(int success, int status, const char *body, size_t len, void *ud)
{
    (void)len;
    op_t *p = (op_t *)ud;
    if (!success || status < 200 || status >= 300) { op_fail(p); return; }

    cJSON *root;
    cJSON *js = parse_js(body, &root);
    if (js && cJSON_IsObject(js)) js = cJSON_GetObjectItem(js, "data");
    if (!js || !cJSON_IsArray(js)) {
        if (root) cJSON_Delete(root);
        g_authed = 0;                        /* session probably expired */
        op_fail(p);
        return;
    }

    g_gen_n = 0;
    int n = cJSON_GetArraySize(js);
    for (int i = 0; i < n && g_gen_n < 512; ++i) {
        cJSON *o = cJSON_GetArrayItem(js, i);
        char id[16], title[64];
        json_str(o, "id", id, sizeof id);
        json_str(o, "title", title, sizeof title);
        if (!id[0] || strcmp(id, "*") == 0) continue;
        snprintf(g_gen_id[g_gen_n], sizeof g_gen_id[0], "%s", id);
        snprintf(g_gen_title[g_gen_n], sizeof g_gen_title[0], "%s", title[0] ? title : id);
        g_gen_n++;
    }
    cJSON_Delete(root);
    emit_genres(p);
}

static void do_resolve(op_t *p);
static void search_channels(op_t *p);
static void run_catalog(op_t *p);

/*
 * The channel list kept on disk, per portal and MAC. 16000 channels took some
 * 17 seconds to download each start; read from disk they are there at once.
 * Fresher than CH_FRESH it is used as it is; older (up to CH_STALE) it is used
 * while the portal sends a new one behind the scenes. Settings > Clear cache
 * removes it with the other lists.
 */
#define CH_FRESH (12 * 3600)
#define CH_STALE (7 * 86400)
static int  g_ch_from_disk;           /* g_ch came from the file: a fresh one is welcome */
static long g_ch_disk_age;

static int ch_cache_path(char *out, size_t sz)
{
    char dir[512];
    if (sc_dir(dir, sizeof dir) != 0) return -1;
    unsigned long long h = sc_fnv(g_portal, 1469598103934665603ull);
    h = sc_fnv("|", h);
    h = sc_fnv(g_mac, h);
    const int w = snprintf(out, sz, "%s/channels_%016llx.tsv", dir, h);
    return (w > 0 && (size_t)w < sz) ? 0 : -1;
}

static void ch_field(char **cur, char *out, size_t sz)
{
    char *t = strchr(*cur, '\t');
    if (t) *t = '\0';
    snprintf(out, sz, "%s", *cur);
    *cur = t ? t + 1 : *cur + strlen(*cur);
}

static void ch_save_disk(void)
{
    char path[600], tmp[620];
    if (g_ch_n <= 0 || ch_cache_path(path, sizeof path) != 0) return;
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "w");
    if (!f) return;
    fprintf(f, "EVOCH1\t%ld\t%d\n", (long)time(NULL), g_ch_n);
    for (int i = 0; i < g_ch_n; ++i) {
        const stalker_ch_t *c = &g_ch[i];
        fprintf(f, "%s\t%s\t%d\t%d\t%d\t%s\t%s\t%s\n", c->id, c->genre, c->num, c->archive,
                c->archive_days, c->name, c->logo, c->cmd);
    }
    fclose(f);
    if (rename(tmp, path) != 0) {
        remove(path);
        if (rename(tmp, path) != 0) remove(tmp);
    }
}

/* 1 when the kept list was read (g_ch filled). */
static int ch_load_disk(void)
{
    char path[600];
    if (g_ch_n > 0 || ch_cache_path(path, sizeof path) != 0) return 0;
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    char line[1024];
    long saved = 0;
    int want = 0;
    if (!fgets(line, sizeof line, f) || sscanf(line, "EVOCH1\t%ld\t%d", &saved, &want) != 2 ||
        want <= 0 || want > MAX_CHANNELS) {
        fclose(f);
        return 0;
    }
    const long age = (long)time(NULL) - saved;
    if (age > CH_STALE || age < -3600) { fclose(f); return 0; }
    stalker_ch_t *chs = (stalker_ch_t *)calloc((size_t)want, sizeof *chs);
    if (!chs) { fclose(f); return 0; }
    int k = 0;
    while (k < want && fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = '\0';
        char *cur = line, num[16], arch[8], days[8];
        stalker_ch_t *c = &chs[k];
        ch_field(&cur, c->id, sizeof c->id);
        ch_field(&cur, c->genre, sizeof c->genre);
        ch_field(&cur, num, sizeof num);
        ch_field(&cur, arch, sizeof arch);
        ch_field(&cur, days, sizeof days);
        ch_field(&cur, c->name, sizeof c->name);
        ch_field(&cur, c->logo, sizeof c->logo);
        ch_field(&cur, c->cmd, sizeof c->cmd);
        if (!c->id[0] || !c->cmd[0]) continue;
        c->num = atoi(num);
        c->archive = atoi(arch);
        c->archive_days = atoi(days);
        k++;
    }
    fclose(f);
    if (k == 0) { free(chs); return 0; }
    free(g_ch);
    g_ch = chs;
    g_ch_n = k;
    g_ch_from_disk = 1;
    g_ch_disk_age = age;
    PROV_LOG("stalker: %d channels from the kept list (%ld min old)", k, age / 60);
    return 1;
}

static void on_channels(int success, int status, const char *body, size_t len, void *ud)
{
    (void)len;
    op_t *p = (op_t *)ud;
    g_ch_loading = 0;
    if (!success || status < 200 || status >= 300) { op_fail(p); return; }

    cJSON *root;
    cJSON *js = parse_js(body, &root);
    cJSON *arr = js;
    if (js && cJSON_IsObject(js)) arr = cJSON_GetObjectItem(js, "data");
    if (!arr || !cJSON_IsArray(arr)) {
        if (root) cJSON_Delete(root);
        g_authed = 0;
        op_fail(p);
        return;
    }

    int n = cJSON_GetArraySize(arr);
    if (n > MAX_CHANNELS) n = MAX_CHANNELS;
    stalker_ch_t *chs = (stalker_ch_t *)calloc((size_t)(n ? n : 1), sizeof *chs);
    if (!chs) { cJSON_Delete(root); op_fail(p); return; }

    int k = 0;
    for (int i = 0; i < n; ++i) {
        cJSON *o = cJSON_GetArrayItem(arr, i);
        stalker_ch_t *c = &chs[k];
        json_str(o, "id", c->id, sizeof c->id);
        json_str(o, "cmd", c->cmd, sizeof c->cmd);
        if (!c->id[0] || !c->cmd[0]) continue;
        json_str(o, "tv_genre_id", c->genre, sizeof c->genre);
        json_str(o, "name", c->name, sizeof c->name);
        evo_title_normalize_spaces(c->name);
        json_str(o, "logo", c->logo, sizeof c->logo);
        json_str(o, "xmltv_id", c->xmltv_id, sizeof c->xmltv_id);
        c->num = json_int(o, "number");
        c->archive = json_int(o, "tv_archive") == 1;
        c->archive_days = json_int(o, "tv_archive_duration");
        k++;
    }
    cJSON_Delete(root);

    free(g_ch);
    g_ch = chs;
    g_ch_n = k;
    g_ch_from_disk = 0;
    PROV_LOG("stalker: %d channels loaded", k);
    ch_save_disk();                               /* the next start has them at once */

    if (p->op == OP_CATALOG && (strncmp(p->arg, "gd:", 3) == 0 || strncmp(p->arg, "gp:", 3) == 0 ||
                                strncmp(p->arg, "ad:", 3) == 0))
        run_catalog(p);                               /* a guide or catch-up day, now that the channels are known */
    else if (p->op == OP_CATALOG) emit_channels(p);
    else if (p->op == OP_SEARCH) search_channels(p);
    else if (p->op == OP_PRELOAD) free(p);            /* only wanted the list in memory */
    else                         do_resolve(p);
}

/* ---- movies and series ------------------------------------------------- */

/* "<t><kind>:..." with t in v/s and kind one of `kinds`. */
static int is_vod_id(const char *id, const char *kinds)
{
    return id && strlen(id) > 3 && id[2] == ':' && (id[0] == 'v' || id[0] == 's') &&
           strchr(kinds, id[1]) != NULL;
}

static void emit_root(evo_provider_items_cb cb, void *ud)
{
    static const char *ids[3]    = { "live", "vod", "series" };
    static const char *titles[3] = { "Live Channels", "Movies", "TV Series" };
    static const char *subs[3]   = { "Live broadcast TV", "Films on demand",
                                     "Series and episodes" };
    evo_provider_item_t items[3];
    for (int i = 0; i < 3; ++i) {
        evo_provider_item_clear(&items[i]);
        snprintf(items[i].id, sizeof items[i].id, "%s", ids[i]);
        snprintf(items[i].title, sizeof items[i].title, "%s", titles[i]);
        snprintf(items[i].subtitle, sizeof items[i].subtitle, "%s", subs[i]);
        items[i].kind = EVO_MEDIA_FOLDER;
        items[i].is_folder = 1;
    }
    if (cb) cb(1, items, 3, 0, ud);
}


/* The parental lock: an adult-named category is remembered, a locked one
 * says so in its name. */
static void parental_mark(const char *provider, evo_provider_item_t *it)
{
    evo_parental_note_category(provider, it->id, it->title);
    if (evo_parental_has_pin() && evo_parental_category_locked(provider, it->id, it->title)) {
        const size_t n = strlen(it->title);
        snprintf(it->title + n, sizeof it->title - n, "  [LOCKED]");
    }
}

static void emit_cats(op_t *p)
{
    int ti = (p->type == 'v') ? 0 : 1;
    int idx[256];
    int k = apply_prefs(ti == 0 ? SC_VOD : SC_SERIES, (const char (*)[16])g_cat_id[ti],
                        g_cat_n[ti], idx);
    evo_provider_item_t *items = (evo_provider_item_t *)calloc((size_t)(k ? k : 1), sizeof *items);
    if (!items) { op_fail(p); return; }

    for (int i = 0; i < k; ++i) {
        int j = idx[i];
        evo_provider_item_clear(&items[i]);
        snprintf(items[i].id, sizeof items[i].id, "%cc:%s", p->type, g_cat_id[ti][j]);
        snprintf(items[i].title, sizeof items[i].title, "%s", g_cat_title[ti][j]);
        parental_mark("stalker", &items[i]);
        snprintf(items[i].subtitle, sizeof items[i].subtitle, "%s",
                 p->type == 'v' ? "Movie category" : "Series category");
        items[i].kind = EVO_MEDIA_FOLDER;
        items[i].is_folder = 1;
    }
    if (p->icb) p->icb(1, items, k, 0, p->ud);
    free(items);
    free(p);
}

static void on_vcats(int success, int status, const char *body, size_t len, void *ud)
{
    (void)len;
    op_t *p = (op_t *)ud;
    if (!success || status < 200 || status >= 300) { op_fail(p); return; }

    cJSON *root;
    cJSON *js = parse_js(body, &root);
    if (js && cJSON_IsObject(js)) js = cJSON_GetObjectItem(js, "data");
    if (!js || !cJSON_IsArray(js)) {
        if (root) cJSON_Delete(root);
        g_authed = 0;
        op_fail(p);
        return;
    }

    int ti = (p->type == 'v') ? 0 : 1;
    g_cat_n[ti] = 0;
    int n = cJSON_GetArraySize(js);
    for (int i = 0; i < n && g_cat_n[ti] < 256; ++i) {
        cJSON *o = cJSON_GetArrayItem(js, i);
        char id[16], title[64];
        json_str(o, "id", id, sizeof id);
        json_str(o, "title", title, sizeof title);
        if (!id[0] || strcmp(id, "*") == 0) continue;
        snprintf(g_cat_id[ti][g_cat_n[ti]], sizeof g_cat_id[0][0], "%s", id);
        snprintf(g_cat_title[ti][g_cat_n[ti]], sizeof g_cat_title[0][0], "%s", title[0] ? title : id);
        g_cat_n[ti]++;
    }
    cJSON_Delete(root);
    emit_cats(p);
}


/* ------------------------------------------------------------------------- */
/* Film links kept on disk                                                    */
/* ------------------------------------------------------------------------- */
/*
 * A film's cmd comes from the list that showed it. Started from Home after a
 * restart (Trending, Continue Watching) that list is gone, and the guess
 * "/media/file_<id>.mpg" is refused by many portals (HTTP 500). The cmd of
 * every film Home can show - search results, the newest lists - is kept in
 * stalker_cmds.txt, per portal, the last CMD_KEEP of them.
 */
#define CMD_FILE "stalker_cmds.txt"
#define CMD_KEEP 4000

static const char *g_cmd_portal;              /* the portal cmd_key is about; NULL = the active one */

static void cmd_key(char t, const char *id, char *out, size_t sz)
{
    unsigned long long h = 1469598103934665603ull;
    for (const char *c = g_cmd_portal ? g_cmd_portal : g_portal; *c; ++c)
        h = (h ^ (unsigned char)*c) * 1099511628211ull;
    snprintf(out, sz, "%016llx|%c:%s", h, t, id);
}

/*
 * The kept links in memory too: every search result and Home lookup asks
 * whether its link is kept, and each ask used to read the whole file (up to
 * 3000 lines). Filled from the file once, kept up to date by cmd_store.
 */
#define CMD_MEM 16384                             /* a power of two, above 2 x CMD_KEEP */
static struct { unsigned long long h; char *cmd; } g_cmdmem[CMD_MEM];
static int g_cmdmem_state;                        /* 0 not loaded, 1 loaded, 2 full: use the file */
static int g_cmdmem_n;

static unsigned long long cmd_hash(const char *s, size_t n)
{
    unsigned long long h = 1469598103934665603ull;
    for (size_t i = 0; i < n; ++i) h = (h ^ (unsigned char)s[i]) * 1099511628211ull;
    return h ? h : 1;
}

static void cmdmem_put(unsigned long long h, const char *cmd)
{
    if (g_cmdmem_state == 2) return;
    for (unsigned i = (unsigned)h & (CMD_MEM - 1), probe = 0; probe < CMD_MEM; ++probe, i = (i + 1) & (CMD_MEM - 1)) {
        if (g_cmdmem[i].h == h) {
            char *d = strdup(cmd);
            if (d) { free(g_cmdmem[i].cmd); g_cmdmem[i].cmd = d; }
            return;
        }
        if (!g_cmdmem[i].h) {
            if (g_cmdmem_n >= CMD_MEM * 3 / 4) { g_cmdmem_state = 2; return; }
            g_cmdmem[i].cmd = strdup(cmd);
            if (!g_cmdmem[i].cmd) { g_cmdmem_state = 2; return; }
            g_cmdmem[i].h = h;
            g_cmdmem_n++;
            return;
        }
    }
    g_cmdmem_state = 2;
}

static void cmdmem_load(void)
{
    if (g_cmdmem_state) return;
    g_cmdmem_state = 1;
    FILE *f = fopen(evo_data_path(CMD_FILE), "r");
    if (!f) return;
    char line[600];
    while (fgets(line, sizeof line, f) && g_cmdmem_state == 1) {
        char *tab = strchr(line, '\t');
        if (!tab) continue;
        line[strcspn(line, "\r\n")] = '\0';
        cmdmem_put(cmd_hash(line, (size_t)(tab - line)), tab + 1);   /* the last one wins */
    }
    fclose(f);
}

static int cmd_lookup_file(char t, const char *id, char *cmd, size_t sz);

static int cmd_lookup(char t, const char *id, char *cmd, size_t sz)
{
    cmdmem_load();
    if (g_cmdmem_state == 2) return cmd_lookup_file(t, id, cmd, sz);
    char key[96];
    cmd_key(t, id, key, sizeof key);
    const unsigned long long h = cmd_hash(key, strlen(key));
    for (unsigned i = (unsigned)h & (CMD_MEM - 1), probe = 0; probe < CMD_MEM; ++probe, i = (i + 1) & (CMD_MEM - 1)) {
        if (!g_cmdmem[i].h) return 0;
        if (g_cmdmem[i].h == h) {
            snprintf(cmd, sz, "%s", g_cmdmem[i].cmd ? g_cmdmem[i].cmd : "");
            return cmd[0] != '\0';
        }
    }
    return 0;
}

static int cmd_lookup_file(char t, const char *id, char *cmd, size_t sz)
{
    char key[96], line[600];
    cmd_key(t, id, key, sizeof key);
    const size_t kl = strlen(key);
    FILE *f = fopen(evo_data_path(CMD_FILE), "r");
    if (!f) return 0;
    int found = 0;
    while (fgets(line, sizeof line, f)) {
        if (strncmp(line, key, kl) != 0 || line[kl] != '\t') continue;
        line[strcspn(line, "\r\n")] = '\0';
        snprintf(cmd, sz, "%s", line + kl + 1);
        found = 1;                                    /* the last one wins: keep reading */
    }
    fclose(f);
    return found && cmd[0];
}

/*
 * The name a film is known by, given by Home / Continue Watching just before
 * it is played. When neither the kept list nor movie_id gives its link, the
 * portal is searched for that name and the row with the same id gives it.
 */
#define THINT_N 8
static struct { char id[EVO_PROVIDER_MAX_ITEM_ID]; char title[128]; } g_thint[THINT_N];
static int g_thint_at;

void stalker_hint_title(const char *item_id, const char *title)
{
    if (!item_id || !title || !title[0]) return;
    const char *bar = strchr(item_id, '|');            /* "0|vm:123" -> "vm:123" */
    const char *id = bar ? bar + 1 : item_id;
    for (int i = 0; i < THINT_N; ++i)
        if (strcmp(g_thint[i].id, id) == 0) {
            snprintf(g_thint[i].title, sizeof g_thint[i].title, "%s", title);
            return;
        }
    snprintf(g_thint[g_thint_at].id, sizeof g_thint[0].id, "%s", id);
    snprintf(g_thint[g_thint_at].title, sizeof g_thint[0].title, "%s", title);
    g_thint_at = (g_thint_at + 1) % THINT_N;
}

static const char *thint_for(const char *id)
{
    for (int i = 0; i < THINT_N; ++i)
        if (g_thint[i].id[0] && strcmp(g_thint[i].id, id) == 0) return g_thint[i].title;
    return NULL;
}

static void cmd_store(char t, const char *id, const char *cmd)
{
    if (!id || !id[0] || !cmd || !cmd[0] || strchr(cmd, '\n')) return;
    char have[400];
    if (cmd_lookup(t, id, have, sizeof have) && strcmp(have, cmd) == 0) return;
    char key[96];
    cmd_key(t, id, key, sizeof key);
    FILE *f = fopen(evo_data_path(CMD_FILE), "a");
    if (!f) return;
    fprintf(f, "%s\t%s\n", key, cmd);
    cmdmem_put(cmd_hash(key, strlen(key)), cmd);
    const long size = ftell(f);
    fclose(f);
    if (size < 2 * CMD_KEEP * 120) return;
    /* Too long: keep the newest CMD_KEEP lines. */
    f = fopen(evo_data_path(CMD_FILE), "r");
    if (!f) return;
    char **lines = (char **)calloc(CMD_KEEP, sizeof *lines);
    int n = 0, at = 0;
    char line[600];
    while (lines && fgets(line, sizeof line, f)) {
        free(lines[at]);
        lines[at] = strdup(line);
        at = (at + 1) % CMD_KEEP;
        if (n < CMD_KEEP) n++;
    }
    fclose(f);
    if (!lines) return;
    char tmp[600];
    snprintf(tmp, sizeof tmp, "%s.tmp", evo_data_path(CMD_FILE));
    f = fopen(tmp, "w");
    if (f) {
        for (int i = 0; i < n; ++i) {
            const char *l = lines[(at - n + i + CMD_KEEP) % CMD_KEEP];
            if (l) fputs(l, f);
        }
        fclose(f);
        rename(tmp, evo_data_path(CMD_FILE));
    }
    for (int i = 0; i < CMD_KEEP; ++i) free(lines[i]);
    free(lines);
}

static void fill_watch_state(evo_provider_item_t *it);

/* Append the items of a get_ordered_list "data" array to items[k..cap). */
static int vlist_items(char type, cJSON *data, const char *parent,
                       evo_provider_item_t *items, int k, int cap)
{
    int n = cJSON_GetArraySize(data);
    for (int i = 0; i < n && k < cap; ++i) {
        cJSON *o = cJSON_GetArrayItem(data, i);
        char id[48], cmd[384], name[EVO_PROVIDER_MAX_TITLE], year[16], art[EVO_PROVIDER_MAX_ART_URL];
        json_str(o, "id", id, sizeof id);
        json_str(o, "cmd", cmd, sizeof cmd);
        json_str(o, "name", name, sizeof name);
        evo_title_normalize_spaces(name);
        json_str(o, "year", year, sizeof year);
        json_str(o, "screenshot_uri", art, sizeof art);
        if (!id[0]) continue;
        if (strcmp(parent, "search") == 0 || strncmp(parent, "vn:", 3) == 0 || strncmp(parent, "sn:", 3) == 0) {
            /* Search and Home: nothing from a locked category (in a category
             * itself the lock was already passed). */
            char cat[32], ck[40];
            json_str(o, "category_id", cat, sizeof cat);
            snprintf(ck, sizeof ck, "%cc:%s", type, cat);
            if (evo_parental_hides("stalker", cat[0] ? ck : "", name)) continue;
        }

        cJSON *sr = cJSON_GetObjectItem(o, "series");
        cJSON *ss = cJSON_GetObjectItem(o, "seasons");
        int is_series = (type == 's') ||
                        (sr && cJSON_IsArray(sr) && cJSON_GetArraySize(sr) > 0) ||
                        (ss && cJSON_IsArray(ss) && cJSON_GetArraySize(ss) > 0) ||
                        json_int(o, "is_series") == 1;
        vc_put(type, id, cmd, o, is_series, is_vod_id(parent, "c") ? parent + 3 : "");
        if (!is_series)
            cmd_store(type, id, cmd);                 /* Home / Continue Watching may start it after a restart */
        {
            vc_t *ve = vc_find(type, id);
            if (ve) {
                snprintf(ve->name, sizeof ve->name, "%s", name);
                if (strncmp(art, "http", 4) == 0)
                    snprintf(ve->art, sizeof ve->art, "%s", art);
                snprintf(ve->year, sizeof ve->year, "%.4s", year);
                json_str(o, "description", ve->desc, sizeof ve->desc);
            }
        }

        evo_provider_item_t *it = &items[k++];
        evo_provider_item_clear(it);
        snprintf(it->id, sizeof it->id, "%c%c:%s", type, is_series ? 's' : 'm', id);
        snprintf(it->parent_id, sizeof it->parent_id, "%s", parent);
        snprintf(it->title, sizeof it->title, "%s", name[0] ? name : "Untitled");
        snprintf(it->subtitle, sizeof it->subtitle, "%s", year);
        json_str(o, "description", it->overview, sizeof it->overview);
        if (strncmp(art, "http", 4) == 0)
            snprintf(it->art_url, sizeof it->art_url, "%s", art);
        if (is_series) {
            it->kind = EVO_MEDIA_FOLDER;
            it->is_folder = 1;
        } else {
            it->kind = EVO_MEDIA_VIDEO;
            it->duration_sec = (int64_t)json_int(o, "time") * 60;
            fill_watch_state(it);                     /* a film: watched / how far in */
        }
    }
    return k;
}

static void on_vlist(int success, int status, const char *body, size_t len, void *ud)
{
    (void)len;
    op_t *p = (op_t *)ud;
    if (!success || status < 200 || status >= 300) { op_fail(p); return; }

    cJSON *root;
    cJSON *js = parse_js(body, &root);
    cJSON *data = (js && cJSON_IsObject(js)) ? cJSON_GetObjectItem(js, "data") : NULL;
    if (!data || !cJSON_IsArray(data)) {
        if (root) cJSON_Delete(root);
        g_authed = 0;
        op_fail(p);
        return;
    }

    int total = json_int(js, "total_items");
    int per   = json_int(js, "max_page_items");
    int n = cJSON_GetArraySize(data);
    if (n > EVO_PROVIDER_PAGE_MAX) n = EVO_PROVIDER_PAGE_MAX;
    evo_provider_item_t *items = (evo_provider_item_t *)calloc((size_t)(n ? n : 1), sizeof *items);
    if (!items) { cJSON_Delete(root); op_fail(p); return; }

    int k = vlist_items(p->type, data, p->arg, items, 0, n);
    cJSON_Delete(root);

    int has_more = (per > 0 && total > 0) ? ((p->page + 1) * per < total)
                                          : (per > 0 && n >= per);
    if (p->icb) p->icb(1, items, k, has_more, p->ud);
    free(items);
    free(p);
}

/*
 * What Continue Watching knows about a row: watched (a check mark), or how far
 * in (a progress bar). The list draws both already, for Emby; a Stalker row
 * just never said.
 */
static void fill_watch_state(evo_provider_item_t *it)
{
    char key[sizeof recent_files[0].path];
    snprintf(key, sizeof key, "evo://stalker/%d|%s", g_active, it->id);
    for (int i = 0; i < recent_file_count; ++i) {
        if (strcmp(recent_files[i].path, key) != 0) continue;
        const double d = recent_files[i].duration, pos = recent_files[i].last_pos;
        if (d > 1.0 && pos / d >= 0.95) {
            it->played = 1;
        } else if (pos > 1.0) {
            it->resume_pos_sec = (int64_t)pos;
            if (it->duration_sec <= 0 && d > 1.0) it->duration_sec = (int64_t)d;
        }
        return;
    }
}

/*
 * "<t>s:<id>" lists a series' seasons (or its episodes when it has no seasons),
 * "<t>z:<id>:<season>" lists one season's episodes. Answered from the cache,
 * no request. Always consumes p.
 */
static void emit_series_children(op_t *p)
{
    char t = p->arg[0], kind = p->arg[1];
    char idbuf[EVO_PROVIDER_MAX_ITEM_ID];
    snprintf(idbuf, sizeof idbuf, "%s", p->arg + 3);

    int si = -1;
    if (kind == 'z') {
        char *c = strrchr(idbuf, ':');
        if (!c) { op_fail(p); return; }
        si = atoi(c + 1);
        *c = '\0';
    }

    vc_t *e = vc_find(t, idbuf);
    if (!e || e->nseasons <= 0 || !e->seasons) { op_fail(p); return; }

    if (kind == 's' && e->has_seasons) {              /* the seasons */
        int n = e->nseasons;
        evo_provider_item_t *items = (evo_provider_item_t *)calloc((size_t)n, sizeof *items);
        if (!items) { op_fail(p); return; }
        for (int i = 0; i < n; ++i) {
            evo_provider_item_clear(&items[i]);
            snprintf(items[i].id, sizeof items[i].id, "%cz:%s:%d", t, idbuf, i);
            snprintf(items[i].parent_id, sizeof items[i].parent_id, "%s", p->arg);
            if (e->seasons[i].name[0])
                snprintf(items[i].title, sizeof items[i].title, "%s", e->seasons[i].name);
            else
                snprintf(items[i].title, sizeof items[i].title, "Season %d", i + 1);
            snprintf(items[i].subtitle, sizeof items[i].subtitle, "%d episodes",
                     e->seasons[i].n);
            items[i].kind = EVO_MEDIA_FOLDER;
            items[i].is_folder = 1;
        }
        if (p->icb) p->icb(1, items, n, 0, p->ud);
        free(items);
        free(p);
        return;
    }

    int sidx = (kind == 'z') ? si : 0;                /* the episodes */
    if (sidx < 0 || sidx >= e->nseasons) { op_fail(p); return; }
    const vseason_t *sn = &e->seasons[sidx];
    int n = sn->n;
    if (n > EVO_PROVIDER_PAGE_MAX) n = EVO_PROVIDER_PAGE_MAX;
    evo_provider_item_t *items = (evo_provider_item_t *)calloc((size_t)(n ? n : 1), sizeof *items);
    if (!items) { op_fail(p); return; }
    for (int i = 0; i < n; ++i) {
        evo_provider_item_clear(&items[i]);
        snprintf(items[i].id, sizeof items[i].id, "%ce:%s:%d:%d", t, idbuf,
                 e->has_seasons ? si : -1, sn->num[i]);
        snprintf(items[i].parent_id, sizeof items[i].parent_id, "%s", p->arg);
        /*
         * The title is what Continue Watching keeps, so it names the series and
         * the episode ("Show - S1E3"), the same shape next / previous episode
         * uses. The episode's own name moves to the subtitle.
         */
        char epname[96];
        if (sn->ename && sn->ename[i])
            snprintf(epname, sizeof epname, "%s", sn->ename[i]);
        else
            snprintf(epname, sizeof epname, "Episode %d", sn->num[i]);
        if (e->name[0]) {
            if (e->has_seasons)
                snprintf(items[i].title, sizeof items[i].title, "%s - S%dE%d",
                         e->name, si >= 0 ? si + 1 : 1, sn->num[i]);
            else
                snprintf(items[i].title, sizeof items[i].title, "%s - E%d",
                         e->name, sn->num[i]);
            snprintf(items[i].subtitle, sizeof items[i].subtitle, "%s", epname);
        } else {
            snprintf(items[i].title, sizeof items[i].title, "%s", epname);
        }
        if (e->art[0])
            snprintf(items[i].art_url, sizeof items[i].art_url, "%s", e->art);
        items[i].kind = EVO_MEDIA_VIDEO;
        fill_watch_state(&items[i]);
    }
    if (p->icb) p->icb(1, items, n, 0, p->ud);
    free(items);
    free(p);
}

/* ---- search ------------------------------------------------------------ */

static int ci_contains(const char *hay, const char *needle)
{
    size_t nl = strlen(needle);
    if (!nl) return 1;
    for (; *hay; ++hay) {
        size_t i = 0;
        while (i < nl && hay[i] &&
               tolower((unsigned char)hay[i]) == tolower((unsigned char)needle[i])) i++;
        if (i == nl) return 1;
    }
    return 0;
}

static void search_next(op_t *p);

static void on_search_list(int success, int status, const char *body, size_t len, void *ud)
{
    (void)len;
    op_t *p = (op_t *)ud;
    if (success && status >= 200 && status < 300) {
        cJSON *root;
        cJSON *js = parse_js(body, &root);
        cJSON *data = (js && cJSON_IsObject(js)) ? cJSON_GetObjectItem(js, "data") : NULL;
        int more = 0;
        if (data && cJSON_IsArray(data)) {
            p->acc_n = vlist_items(p->type, data, "search", p->acc, p->acc_n,
                                   EVO_PROVIDER_PAGE_MAX);
            /* Home looks for every version of a title: the next pages too. */
            const int total = json_int(js, "total_items"), per = json_int(js, "max_page_items");
            more = p->only && per > 0 && total > (p->page + 1) * per && p->page < 3 &&
                   cJSON_GetArraySize(data) > 0;
        }
        if (root) cJSON_Delete(root);
        if (more) { p->page++; search_next(p); return; }
    }
    p->stage++;                       /* a failed stage just contributes nothing */
    p->page = 0;
    search_next(p);
}

static void search_next(op_t *p)
{
    while (p->stage <= (p->only ? p->only : 2)) {
        char esc[480], q[640];
        p->type = (p->stage == 1) ? 'v' : 's';
        if (p->acc_n < EVO_PROVIDER_PAGE_MAX &&
            evo_provider_url_escape(p->arg, esc, sizeof esc) >= 0) {
            snprintf(q, sizeof q,
                     "type=%s&action=get_ordered_list&search=%s&p=%d&sortby=added",
                     p->type == 'v' ? "vod" : "series", esc, p->page + 1);
            if (api_get(q, on_search_list, p, 1) == 0) return;
        }
        p->stage++;
    }
    evo_provider_item_t *acc = p->acc;
    int n = p->acc_n;
    p->acc = NULL;
    if (p->icb) p->icb(1, acc, n, 0, p->ud);
    free(acc);
    free(p);
}

/* Stage 0: live channels, matched here against the cached list. */
static void search_channels(op_t *p)
{
    for (int i = 0; i < g_ch_n && p->acc_n < EVO_PROVIDER_PAGE_MAX; ++i) {
        const stalker_ch_t *c = &g_ch[i];
        if (!ci_contains(c->name, p->arg)) continue;
        {
            char gk[40];
            snprintf(gk, sizeof gk, "g:%s", c->genre);
            if (evo_parental_hides("stalker", c->genre[0] ? gk : "", c->name)) continue;
        }
        evo_provider_item_t *it = &p->acc[p->acc_n++];
        evo_provider_item_clear(it);
        snprintf(it->id, sizeof it->id, "ch:%s", c->id);
        snprintf(it->parent_id, sizeof it->parent_id, "search");
        snprintf(it->title, sizeof it->title, "%s", c->name[0] ? c->name : "Channel");
        snprintf(it->subtitle, sizeof it->subtitle, "Live Broadcast");
        if (strncmp(c->logo, "http", 4) == 0)
            snprintf(it->art_url, sizeof it->art_url, "%s", c->logo);
        it->kind = EVO_MEDIA_STREAM;
        it->is_live = 1;
    }
    p->stage = 1;
    search_next(p);
}

static void run_search(op_t *p)
{
    p->acc = (evo_provider_item_t *)calloc(EVO_PROVIDER_PAGE_MAX, sizeof *p->acc);
    if (!p->acc) { op_fail(p); return; }
    if (p->only) { p->stage = p->only; search_next(p); return; }   /* films or series, no channels */
    if (g_ch_n > 0) { search_channels(p); return; }
    if (api_get("type=itv&action=get_all_channels", on_channels, p, 1) != 0)
        op_fail(p);
}

/* ---- series whose seasons and episodes are not in the list ------------- */

static void log_keys(const char *what, const cJSON *o)
{
    char keys[200] = "";
    size_t n = 0;
    for (const cJSON *c = o ? o->child : NULL; c && n + 24 < sizeof keys; c = c->next) {
        if (!c->string) continue;
        n += (size_t)snprintf(keys + n, sizeof keys - n, "%s%s", n ? "," : "", c->string);
    }
    PROV_LOG("stalker: %s keys [%s]", what, keys);
}

static int colons(const char *s)
{
    int n = 0;
    for (; *s; ++s)
        if (*s == ':') n++;
    return n;
}

/* A season ("1234:1", or one that lists its episode numbers) or an episode ("1234:1:5")? */
static int el_is_season(const cJSON *el)
{
    char id[48];
    json_str(el, "id", id, sizeof id);
    if (json_int(el, "is_episode") == 1) return 0;
    if (json_int(el, "is_season") == 1)  return 1;
    int c = colons(id);
    if (c >= 2) return 0;
    if (c == 1) return 1;
    return cJSON_GetObjectItem((cJSON *)el, "series") != NULL;
}

/* The js.data of a movie_id request: the seasons, or the episodes when there are no seasons. */
static void vc_from_movie(vc_t *e, cJSON *data)
{
    int n = cJSON_GetArraySize(data);
    if (n <= 0) return;
    for (int i = 0; i < e->nseasons; ++i) vseason_free_eps(&e->seasons[i]);
    free(e->seasons);
    e->seasons = NULL;
    e->nseasons = e->has_seasons = 0;

    if (el_is_season(cJSON_GetArrayItem(data, 0))) {
        int ns = n > VC_MAX_SEASON ? VC_MAX_SEASON : n;
        e->seasons = (vseason_t *)calloc((size_t)ns, sizeof(vseason_t));
        if (!e->seasons) return;
        e->nseasons = ns;
        e->has_seasons = 1;
        for (int i = 0; i < ns; ++i) {
            cJSON *el = cJSON_GetArrayItem(data, i);
            vseason_t *sn = &e->seasons[i];
            json_str(el, "name", sn->name, sizeof sn->name);
            json_str(el, "cmd", sn->cmd, sizeof sn->cmd);
            json_str(el, "id", sn->sid, sizeof sn->sid);
            vseason_fill(sn, cJSON_GetObjectItem(el, "series"));
        }
    } else {
        e->seasons = (vseason_t *)calloc(1, sizeof(vseason_t));
        if (!e->seasons) return;
        e->nseasons = 1;
        vseason_fill(&e->seasons[0], data);
    }
}

/* "<t>s:<id>" or "<t>z:<id>:<season>": is what it lists known already? */
static int series_ready(const char *inner)
{
    char t = inner[0], kind = inner[1];
    char idbuf[EVO_PROVIDER_MAX_ITEM_ID];
    snprintf(idbuf, sizeof idbuf, "%s", inner + 3);
    int si = -1;
    if (kind == 'z') {
        char *c = strrchr(idbuf, ':');
        if (!c) return 1;
        si = atoi(c + 1);
        *c = '\0';
    }
    vc_t *e = vc_find(t, idbuf);
    if (!e) return 0;
    if (kind == 's') {
        if (e->fetched) return 1;
        if (e->nseasons <= 0 || !e->seasons) return 0;
        return e->has_seasons ? 1 : e->seasons[0].n > 0;
    }
    if (si < 0 || si >= e->nseasons) return 1;
    const vseason_t *sn = &e->seasons[si];
    return sn->n > 0 || !sn->sid[0] || sn->fetched;
}

static void on_movie(int success, int status, const char *body, size_t len, void *ud)
{
    (void)len;
    op_t *p = (op_t *)ud;
    char t = p->arg[0], kind = p->arg[1];
    char idbuf[EVO_PROVIDER_MAX_ITEM_ID];
    snprintf(idbuf, sizeof idbuf, "%s", p->arg + 3);
    int si = -1;
    if (kind == 'z') {
        char *c = strrchr(idbuf, ':');
        if (!c) { op_fail(p); return; }
        si = atoi(c + 1);
        *c = '\0';
    }

    cJSON *root = NULL, *data = NULL;
    if (success && status >= 200 && status < 300) {
        cJSON *js = parse_js(body, &root);
        if (js && cJSON_IsObject(js))     data = cJSON_GetObjectItem(js, "data");
        else if (js && cJSON_IsArray(js)) data = js;
    }
    vc_t *e = vc_find(t, idbuf);
    if (!e || !data || !cJSON_IsArray(data)) {
        PROV_LOG("stalker: series request http=%d gave no list", status);
        if (root) cJSON_Delete(root);
        op_fail(p);
        return;
    }
    e->fetched = 1;
    PROV_LOG("stalker: series request gave %d item(s)", cJSON_GetArraySize(data));
    if (cJSON_GetArraySize(data) > 0) log_keys("series item", cJSON_GetArrayItem(data, 0));

    if (kind == 's')                          vc_from_movie(e, data);
    else if (si >= 0 && si < e->nseasons) {
        vseason_fill(&e->seasons[si], data);
        e->seasons[si].fetched = 1;
    }
    cJSON_Delete(root);
    emit_series_children(p);
}

static void fetch_series(op_t *p)
{
    char t = p->arg[0], kind = p->arg[1];
    char idbuf[EVO_PROVIDER_MAX_ITEM_ID], sid[32] = "0";
    snprintf(idbuf, sizeof idbuf, "%s", p->arg + 3);
    int si = -1;
    if (kind == 'z') {
        char *c = strrchr(idbuf, ':');
        if (!c) { op_fail(p); return; }
        si = atoi(c + 1);
        *c = '\0';
    }
    vc_t *e = vc_find(t, idbuf);
    if (kind == 'z') {
        if (!e || si < 0 || si >= e->nseasons || !e->seasons[si].sid[0]) { op_fail(p); return; }
        snprintf(sid, sizeof sid, "%s", e->seasons[si].sid);
    }
    char cat[16] = "";
    if (e) snprintf(cat, sizeof cat, "%s", e->cat);
    else   vc_put(t, idbuf, "", NULL, 0, "");        /* opened from Recent: only the id is known */

    char eid[96], esid[64], ecat[48], q[420];
    if (evo_provider_url_escape(idbuf, eid, sizeof eid) < 0 ||
        evo_provider_url_escape(sid, esid, sizeof esid) < 0 ||
        evo_provider_url_escape(cat, ecat, sizeof ecat) < 0) {
        op_fail(p);
        return;
    }
    snprintf(q, sizeof q,
             "type=%s&action=get_ordered_list&movie_id=%s&season_id=%s&episode_id=0"
             "&category=%s&fav=0&p=1&sortby=added",
             t == 'v' ? "vod" : "series", eid, esid, ecat);
    PROV_LOG("stalker: asking the portal for the %s of a series", kind == 'z' ? "episodes" : "seasons");
    if (api_get_cached(q, on_movie, p) != 0) op_fail(p);
}


/* ------------------------------------------------------------------------- */
/* Catch-up: programmes already broadcast, from the portal's archive          */
/* ------------------------------------------------------------------------- */
/*
 * Live TV -> "Catch-up" lists the channels the portal keeps an archive for
 * (tv_archive=1). A channel opens on its days ("ar:<ch>"), a day on that day's
 * programmes that are over and archived ("ad:<ch>:<YYYY-MM-DD>", from
 * epg/get_simple_data_table), and a programme ("ap:<ch>:<programme id>")
 * plays through tv_archive/create_link - a recording, so it can be paused and
 * seeked and is kept in Continue Watching like a film.
 */
#define ARCHIVE_DAYS_DEFAULT 7
static void start_link(op_t *p, const char *type, const char *cmd, const char *series);
#define ARCHIVE_DAYS_MAX     14

static int channel_archive_days(const stalker_ch_t *c)
{
    int d = c->archive_days;
    if (d > 30) d /= 24;                      /* some portals give hours */
    if (d <= 0) d = ARCHIVE_DAYS_DEFAULT;
    return d > ARCHIVE_DAYS_MAX ? ARCHIVE_DAYS_MAX : d;
}

static const stalker_ch_t *channel_by_id(const char *id)
{
    for (int i = 0; i < g_ch_n; ++i)
        if (strcmp(g_ch[i].id, id) == 0) return &g_ch[i];
    return NULL;
}

/* "ar:<ch>": today and the days before it, as far back as the archive goes. */
static void emit_archive_days(op_t *p)
{
    const char *ch = p->arg + 3;
    const stalker_ch_t *c = channel_by_id(ch);
    const int days = c ? channel_archive_days(c) : ARCHIVE_DAYS_DEFAULT;
    evo_provider_item_t *items = (evo_provider_item_t *)calloc((size_t)days, sizeof *items);
    if (!items) { op_fail(p); return; }
    static const char *wd[7] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
    const time_t now = time(NULL);
    for (int i = 0; i < days; ++i) {
        time_t t = now - (time_t)i * 86400;
        struct tm tmv;
        localtime_r(&t, &tmv);
        char date[16];
        snprintf(date, sizeof date, "%04d-%02d-%02d", tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday);
        evo_provider_item_clear(&items[i]);
        snprintf(items[i].id, sizeof items[i].id, "ad:%s:%s", ch, date);
        snprintf(items[i].parent_id, sizeof items[i].parent_id, "%s", p->arg);
        if (i == 0)      snprintf(items[i].title, sizeof items[i].title, "Today");
        else if (i == 1) snprintf(items[i].title, sizeof items[i].title, "Yesterday");
        else snprintf(items[i].title, sizeof items[i].title, "%s %02d-%02d", wd[tmv.tm_wday % 7],
                      tmv.tm_mday, tmv.tm_mon + 1);
        snprintf(items[i].subtitle, sizeof items[i].subtitle, "%s", c ? c->name : date);
        items[i].kind = EVO_MEDIA_FOLDER;
        items[i].is_folder = 1;
    }
    if (p->icb) p->icb(1, items, days, 0, p->ud);
    free(items);
    free(p);
}

/*
 * TV guide: "gd:<ch>" lists the days (today first, then the next three, then
 * the days the archive still has), "gp:<ch>:<date>" a day's programmes. What
 * is on now plays the channel, what is in the archive plays as catch-up, the
 * rest says when it is on.
 */
static void emit_guide_days(op_t *p)
{
    const char *ch = p->arg + 3;
    const stalker_ch_t *c = channel_by_id(ch);
    const int past = (c && c->archive) ? channel_archive_days(c) - 1 : 0;
    const int ahead = 3;
    const int n = 1 + ahead + (past > 0 ? past : 0);
    evo_provider_item_t *items = (evo_provider_item_t *)calloc((size_t)n, sizeof *items);
    if (!items) { op_fail(p); return; }
    static const char *wd[7] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
    const time_t now = time(NULL);
    for (int i = 0; i < n; ++i) {
        /* 0 today, 1..ahead the days after, then yesterday and back */
        const int offset = (i <= ahead) ? i : -(i - ahead);
        time_t t = now + (time_t)offset * 86400;
        struct tm tmv;
        localtime_r(&t, &tmv);
        char date[16];
        snprintf(date, sizeof date, "%04d-%02d-%02d", tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday);
        evo_provider_item_clear(&items[i]);
        snprintf(items[i].id, sizeof items[i].id, "gp:%s:%s", ch, date);
        snprintf(items[i].parent_id, sizeof items[i].parent_id, "%s", p->arg);
        if (offset == 0)       snprintf(items[i].title, sizeof items[i].title, "Today");
        else if (offset == 1)  snprintf(items[i].title, sizeof items[i].title, "Tomorrow");
        else if (offset == -1) snprintf(items[i].title, sizeof items[i].title, "Yesterday");
        else snprintf(items[i].title, sizeof items[i].title, "%s %02d-%02d", wd[tmv.tm_wday % 7],
                      tmv.tm_mday, tmv.tm_mon + 1);
        snprintf(items[i].subtitle, sizeof items[i].subtitle, "%s%s", c ? c->name : date,
                 offset < 0 ? "  -  catch-up" : "");
        items[i].kind = EVO_MEDIA_FOLDER;
        items[i].is_folder = 1;
    }
    if (p->icb) p->icb(1, items, n, 0, p->ud);
    free(items);
    free(p);
}

static void on_archive_day(int success, int status, const char *body, size_t len, void *ud)
{
    (void)len;
    op_t *p = (op_t *)ud;
    if (!success || status < 200 || status >= 300) { op_fail(p); return; }
    cJSON *root = NULL;
    cJSON *js = parse_js(body, &root);
    cJSON *data = (js && cJSON_IsObject(js)) ? cJSON_GetObjectItem(js, "data") : NULL;
    if (!data || !cJSON_IsArray(data)) {
        PROV_LOG("stalker: catch-up day http=%d gave no list", status);
        if (root) cJSON_Delete(root);
        op_fail(p);
        return;
    }
    char ch[32];
    snprintf(ch, sizeof ch, "%s", p->arg + 3);
    char *colon = strchr(ch, ':');
    if (colon) *colon = '\0';
    const stalker_ch_t *c = channel_by_id(ch);

    const int total = json_int(js, "total_items"), per = json_int(js, "max_page_items");
    const int n = cJSON_GetArraySize(data);
    evo_provider_item_t *items = (evo_provider_item_t *)calloc((size_t)(n ? n : 1), sizeof *items);
    if (!items) { cJSON_Delete(root); op_fail(p); return; }
    const long now = (long)time(NULL);
    const int guide = strncmp(p->arg, "gp:", 3) == 0;
    int k = 0;
    for (int i = 0; i < n && k < EVO_PROVIDER_PAGE_MAX; ++i) {
        cJSON *o = cJSON_GetArrayItem(data, i);
        char pid[32], name[96], at[16];
        json_str(o, "id", pid, sizeof pid);
        json_str(o, "name", name, sizeof name);
        evo_title_normalize_spaces(name);
        json_str(o, "t_time", at, sizeof at);
        const long a = json_long(o, "start_timestamp"), b = json_long(o, "stop_timestamp");
        const int archived = json_int(o, "mark_archive") == 1;
        if (guide) {
            if (!name[0]) continue;
            evo_provider_item_t *it = &items[k++];
            evo_provider_item_clear(it);
            snprintf(it->parent_id, sizeof it->parent_id, "%s", p->arg);
            const int on_now = a > 0 && b > 0 && a <= now && now < b;
            if (on_now) {
                snprintf(it->id, sizeof it->id, "ch:%s", ch);           /* watch it live */
                snprintf(it->title, sizeof it->title, "NOW  %s  %s", at, name);
                it->is_live = 1;
                it->kind = EVO_MEDIA_STREAM;
            } else if (archived && b > 0 && b <= now && pid[0] && c && c->archive) {
                snprintf(it->id, sizeof it->id, "ap:%s:%s", ch, pid);    /* catch-up */
                snprintf(it->title, sizeof it->title, "%s  %s", at, name);
                it->kind = EVO_MEDIA_VIDEO;
                if (a > 0 && b > a) it->duration_sec = (int64_t)(b - a);
                fill_watch_state(it);
            } else {
                snprintf(it->id, sizeof it->id, "gi:%s:%ld:%s", ch, a, b > 0 && b <= now ? "past" : "later");
                snprintf(it->title, sizeof it->title, "%s  %s", at, name);
                it->kind = EVO_MEDIA_VIDEO;
            }
            snprintf(it->subtitle, sizeof it->subtitle, "%s", on_now ? "On now - press to watch"
                     : (it->id[0] == 'a') ? "Catch-up - press to watch" : (c ? c->name : "TV guide"));
            json_str(o, "descr", it->overview, sizeof it->overview);
            if (c && strncmp(c->logo, "http", 4) == 0)
                snprintf(it->art_url, sizeof it->art_url, "%s", c->logo);
            continue;
        }
        if (!pid[0] || !name[0] || !archived || (b > 0 && b > now)) continue;   /* not (yet) in the archive */
        evo_provider_item_t *it = &items[k++];
        evo_provider_item_clear(it);
        snprintf(it->id, sizeof it->id, "ap:%s:%s", ch, pid);
        snprintf(it->parent_id, sizeof it->parent_id, "%s", p->arg);
        if (at[0]) snprintf(it->title, sizeof it->title, "%s  %s", at, name);
        else       snprintf(it->title, sizeof it->title, "%s", name);
        snprintf(it->subtitle, sizeof it->subtitle, "%s", c ? c->name : "Catch-up");
        json_str(o, "descr", it->overview, sizeof it->overview);
        if (c && strncmp(c->logo, "http", 4) == 0)
            snprintf(it->art_url, sizeof it->art_url, "%s", c->logo);
        if (a > 0 && b > a) it->duration_sec = (int64_t)(b - a);
        it->kind = EVO_MEDIA_VIDEO;
        fill_watch_state(it);
    }
    cJSON_Delete(root);
    const int has_more = (per > 0 && total > 0) ? ((p->page + 1) * per < total) : 0;
    PROV_LOG("stalker: catch-up %s: %d programme(s) on page %d", p->arg, k, p->page + 1);
    if (p->icb) p->icb(1, items, k, has_more, p->ud);
    free(items);
    free(p);
}

/* "ad:<ch>:<date>": that day's programmes. */
static void fetch_archive_day(op_t *p)
{
    char ch[32], date[16], q[200];
    const char *rest = p->arg + 3;
    const char *colon = strchr(rest, ':');
    if (!colon || (size_t)(colon - rest) >= sizeof ch) { op_fail(p); return; }
    snprintf(ch, sizeof ch, "%.*s", (int)(colon - rest), rest);
    snprintf(date, sizeof date, "%s", colon + 1);
    snprintf(q, sizeof q, "type=epg&action=get_simple_data_table&ch_id=%s&date=%s&p=%d",
             ch, date, p->page + 1);
    if (api_get(q, on_archive_day, p, 1) != 0) op_fail(p);
}

/* "ap:<ch>:<programme>": a link to the recording. */
static void resolve_archive(op_t *p)
{
    const char *pid = strrchr(p->arg, ':');
    if (!pid || !pid[1]) { op_fail(p); return; }
    char cmd[96];
    snprintf(cmd, sizeof cmd, "auto /media/%s.mpg", pid + 1);
    p->live = 0;
    PROV_LOG("stalker: playing a catch-up programme");
    start_link(p, "tv_archive", cmd, "");
}

static void on_channels(int success, int status, const char *body, size_t len, void *ud);

static void run_catalog(op_t *p)
{
    int rc = -1;
    char q[256];
    if (strcmp(p->arg, "live") == 0) {                /* live: the genres */
        if (g_gen_n > 0) { emit_genres(p); return; }
        rc = api_get_cached("type=itv&action=get_genres", on_genres, p);
    } else if (strcmp(p->arg, "vod") == 0 || strcmp(p->arg, "series") == 0) {
        p->type = p->arg[0] == 'v' ? 'v' : 's';
        if (g_cat_n[p->type == 'v' ? 0 : 1] > 0) { emit_cats(p); return; }
        snprintf(q, sizeof q, "type=%s&action=get_categories", p->arg);
        rc = api_get_cached(q, on_vcats, p);
    } else if (strncmp(p->arg, "g:", 2) == 0) {       /* one live genre */
        if (g_ch_n > 0) { emit_channels(p); return; }
        rc = api_get("type=itv&action=get_all_channels", on_channels, p, 1);
    } else if (strncmp(p->arg, "ar:", 3) == 0) {      /* catch-up: a channel's days */
        emit_archive_days(p);
        return;
    } else if (strncmp(p->arg, "ad:", 3) == 0 ||     /* catch-up: one day's programmes */
               strncmp(p->arg, "gp:", 3) == 0) {     /* TV guide: one day */
        if (g_ch_n == 0 && p->stage != 99) {          /* the channel's name and archive, once */
            p->stage = 99;
            rc = api_get("type=itv&action=get_all_channels", on_channels, p, 1);
            if (rc != 0) op_fail(p);
            return;
        }
        fetch_archive_day(p);
        return;
    } else if (strncmp(p->arg, "gd:", 3) == 0) {      /* TV guide: the days */
        if (g_ch_n == 0 && p->stage != 99) {
            p->stage = 99;
            rc = api_get("type=itv&action=get_all_channels", on_channels, p, 1);
            if (rc != 0) op_fail(p);
            return;
        }
        emit_guide_days(p);
        return;
    } else if (is_vod_id(p->arg, "sz")) {             /* a series or season not known yet */
        fetch_series(p);
        return;
    } else if (is_vod_id(p->arg, "n")) {              /* Home: the newest films / series */
        p->type = p->arg[0];
        snprintf(q, sizeof q,
                 "type=%s&action=get_ordered_list&category=*&p=1&sortby=added",
                 p->type == 'v' ? "vod" : "series");
        rc = api_get_cached(q, on_vlist, p);       /* kept a day, like every list */
    } else if (is_vod_id(p->arg, "c")) {              /* a movie / series category */
        char cat[48];
        if (evo_provider_url_escape(p->arg + 3, cat, sizeof cat) >= 0) {
            p->type = p->arg[0];
            snprintf(q, sizeof q,
                     "type=%s&action=get_ordered_list&category=%s&p=%d&sortby=%s",
                     p->type == 'v' ? "vod" : "series", cat, p->page + 1,
                     k_vod_by[g_sort[p->type == 'v' ? SC_VOD : SC_SERIES]]);
            rc = api_get_cached(q, on_vlist, p);
        }
    }
    if (rc != 0) op_fail(p);
}

/* ---- resolve ----------------------------------------------------------- */

static void start_link(op_t *p, const char *type, const char *cmd, const char *series);
static void item_art_url(const char *arg, char *out, size_t sz);
static void item_info(const char *arg, evo_stream_choice_t *c);

/* What a stream URL looks like, without anything that identifies the account. */
static void log_url_shape(const char *url)
{
    const char *h = strstr(url, "://");
    h = h ? h + 3 : url;
    const char *path = strchr(h, '/');
    const char *q = path ? strchr(path, '?') : NULL;
    int segs = 0;
    for (const char *c = path ? path : ""; *c && c != q; ++c)
        if (*c == '/') segs++;
    const char *end = q ? q : path + (path ? strlen(path) : 0);
    const char *dot = NULL;
    for (const char *c = path ? path : ""; c < end; ++c)
        if (*c == '.') dot = c;
    char keys[160] = "";
    if (q) {
        size_t n = 0;
        for (const char *c = q + 1; *c && n + 2 < sizeof keys; ++c) {
            if (*c == '=') {                         /* skip the value */
                while (*c && *c != '&') ++c;
                if (n + 1 < sizeof keys) keys[n++] = ',';
                if (!*c) break;
                continue;
            }
            if (*c != '&') keys[n++] = *c;
        }
        keys[n] = '\0';
    }
    if (keys[0] && keys[strlen(keys) - 1] == ',') keys[strlen(keys) - 1] = '\0';
    char ext[10] = "none";
    if (dot && end - dot < 8) snprintf(ext, sizeof ext, "%.*s", (int)(end - dot), dot);
    PROV_LOG("stalker: stream url: %s, %d path part(s), ext %s, query keys [%s]",
             strncasecmp(url, "https", 5) == 0 ? "https" : "http", segs, ext, keys);
}

static void resolve_search_movie(op_t *p);

static void on_link(int success, int status, const char *body, size_t len, void *ud)
{
    (void)len;
    op_t *p = (op_t *)ud;

    char cmd[EVO_PROVIDER_MAX_URL] = {0};
    if (success && status >= 200 && status < 300) {
        cJSON *root;
        cJSON *js = parse_js(body, &root);
        if (js) json_str(js, "cmd", cmd, sizeof cmd);
        if (js && !cmd[0]) {                          /* say what the portal said instead */
            char why[80];
            log_keys("create_link answer", js);
            json_str(js, "error", why, sizeof why);
            if (!why[0]) json_str(js, "msg", why, sizeof why);
            if (!why[0]) json_str(js, "message", why, sizeof why);
            for (char *c = why; *c; ++c)
                if ((unsigned char)*c < 32) *c = ' ';
            if (why[0]) PROV_LOG("stalker: the portal says: %s", why);
        }
        if (root) cJSON_Delete(root);
    }
    PROV_LOG("stalker: create_link http=%d %s", status, cmd[0] ? "gave a link" : "gave no link");

    const char *url = strip_cmd(cmd);
    if (strncasecmp(url, "http", 4) != 0 && !p->live && !p->alt_tried && p->arg[0] == 's' &&
        strcmp(p->ltype, "vod") == 0) {
        /* A series: portals differ in whether create_link is a vod or a series call. */
        char cmd2[384], ser2[16];
        snprintf(cmd2, sizeof cmd2, "%s", p->cmd);
        snprintf(ser2, sizeof ser2, "%s", p->lser);
        p->alt_tried = 1;
        PROV_LOG("stalker: no link as type=vod, trying type=series");
        start_link(p, "series", cmd2, ser2);
        return;
    }
    if (strncasecmp(url, "http", 4) != 0 && !p->live && p->arg[1] == 'm' && !p->searched &&
        thint_for(p->arg) && (p->cached_cmd || p->refetched)) {
        /* The film's kept link was turned down (HTTP 500 - the portal moved the
         * file): ask the portal for it by name, then try once more. */
        PROV_LOG("stalker: the film's kept link did not work - looking it up by name");
        p->searched = 1;
        resolve_search_movie(p);
        return;
    }
    if (strncasecmp(url, "http", 4) != 0) {
        /* create_link gave nothing usable: a channel whose cmd is already a
         * direct link (not the portal's localhost placeholder) still plays. */
        const char *direct = strip_cmd(p->cmd);
        if (strncasecmp(direct, "http", 4) == 0 && !strstr(direct, "localhost"))
            url = direct;
        else { op_fail(p); return; }
    }
    log_url_shape(url);

    /*
     * Some portals (panels that imitate Stalker) answer create_link for a live
     * channel with "...live.php?mac=..&stream=&extension=ts&play_token=..": the
     * stream id is empty, and the stream server turns that away with a 405. The
     * id is the channel's own: the number after /ch/ in its cmd, else its id.
     */
    char fixed[EVO_PROVIDER_MAX_URL];
    if (p->live && strncmp(p->arg, "ch:", 3) == 0) {   /* what zapping starts from */
        rch_push(g_active, p->arg + 3);
        g_play_portal = g_active;
        snprintf(g_play_id, sizeof g_play_id, "%s", p->arg + 3);
        snprintf(g_play_want, sizeof g_play_want, "%s", g_last_portal == g_active ? g_last_want : "*");
    }
    if (p->live) {
        const char *empty = strstr(url, "stream=&");
        int at_end = !empty && strlen(url) >= 7 && strcmp(url + strlen(url) - 7, "stream=") == 0;
        if (empty || at_end) {
            char chid[24] = "";
            const char *slash = strrchr(p->cmd, '/');            /* ".../ch/1358" */
            if (slash && isdigit((unsigned char)slash[1])) snprintf(chid, sizeof chid, "%.20s", slash + 1);
            if (!chid[0] && strncmp(p->arg, "ch:", 3) == 0) snprintf(chid, sizeof chid, "%.20s", p->arg + 3);
            for (char *d = chid; *d; ++d)                          /* digits only */
                if (!isdigit((unsigned char)*d)) { *d = '\0'; break; }
            if (chid[0]) {
                size_t head = (size_t)((empty ? empty + 7 : url + strlen(url)) - url);
                snprintf(fixed, sizeof fixed, "%.*s%s%s", (int)head, url, chid,
                         empty ? empty + 7 : "");
                url = fixed;
                PROV_LOG("stalker: the link had an empty stream id; filled in the channel's own");
            }
        }
    }

    if (p->live) snprintf(g_play_url, sizeof g_play_url, "%s", url);
    if (!p->live && p->arg[1] == 'm' && p->cmd[0]) {
        /* A film that played: its link is kept, so Continue Watching can
         * start it again after a restart without guessing. */
        cmd_store(p->arg[0], p->arg + 3, p->cmd);
    }
    if (!p->live && is_vod_id(p->arg, "e")) {            /* what next / previous episode starts from */
        g_play_ep_portal = g_active;
        snprintf(g_play_ep, sizeof g_play_ep, "%s", p->arg);
        snprintf(g_play_ep_url, sizeof g_play_ep_url, "%s", url);
    }

    evo_stream_choice_t choices[3];
    int n = 0;
    evo_provider_stream_choice_clear(&choices[0]);
    evo_stream_choice_t *c = &choices[0];
    snprintf(c->url, sizeof c->url, "%s", url);
    snprintf(c->label, sizeof c->label, "%s", p->live ? "Live" : "Direct");
    {
        const char *ct = p->live ? "ts" : "mp4";
        if (strstr(url, ".m3u8"))     ct = "m3u8";
        else if (strstr(url, ".mkv")) ct = "mkv";
        else if (strstr(url, ".avi")) ct = "avi";
        else if (!p->live && strstr(url, ".ts")) ct = "ts";
        snprintf(c->container, sizeof c->container, "%s", ct);
    }
    c->is_live = p->live ? 1 : 0;
    c->max_connections = 1;           /* the panel cuts a second connection on one MAC */
    if (!p->live) {
        item_art_url(p->arg, c->art_url, sizeof c->art_url);
        item_info(p->arg, c);
    }
    snprintf(c->headers, sizeof c->headers,
             "User-Agent: %s\r\nCookie: mac=%s\r\n", STB_UA, g_mac);
    n = 1;

    if (p->rcb) p->rcb(1, choices, n, p->ud);
    free(p);
}

/* create_link for any kind of item. `type` is itv, vod or series. */
static void start_link(op_t *p, const char *type, const char *cmd, const char *series)
{
    snprintf(p->cmd, sizeof p->cmd, "%s", cmd);
    snprintf(p->ltype, sizeof p->ltype, "%s", type);
    snprintf(p->lser, sizeof p->lser, "%s", series ? series : "");
    if (p->live) {                                    /* the placeholder, long opaque runs masked */
        char shape[120];
        size_t n = 0;
        for (const char *c = cmd; *c && n + 2 < sizeof shape; ) {
            size_t run = 0;
            while (isalnum((unsigned char)c[run])) run++;
            if (run >= 12) { shape[n++] = '*'; c += run; }
            else shape[n++] = *c++;
        }
        shape[n] = '\0';
        PROV_LOG("stalker: channel cmd: %s", shape);
    }

    char esc[1280], ser[48];
    if (evo_provider_url_escape(cmd, esc, sizeof esc) < 0 ||
        evo_provider_url_escape(series ? series : "", ser, sizeof ser) < 0) {
        op_fail(p);
        return;
    }
    char q[1700];
    snprintf(q, sizeof q,
             "type=%s&action=create_link&cmd=%s&series=%s&forced_storage=undefined"
             "&disable_ad=0&download=0", type, esc, ser);
    if (api_get(q, on_link, p, 1) != 0) op_fail(p);
}

static void do_resolve(op_t *p)
{
    const char *id = p->arg + 3;                      /* after "ch:" */
    const stalker_ch_t *c = NULL;
    for (int i = 0; i < g_ch_n; ++i)
        if (strcmp(g_ch[i].id, id) == 0) { c = &g_ch[i]; break; }
    if (!c) {
        PROV_LOG("stalker: channel %s is not in the loaded list", id);
        op_fail(p);
        return;
    }

    p->live = 1;
    start_link(p, "itv", c->cmd, "");
}

/* The series an episode id belongs to: "se:<id>:<season>:<ep>" -> "<id>". */
static void episode_series_id(const char *arg, char *out, size_t sz)
{
    snprintf(out, sz, "%s", arg + 3);
    char *c = strrchr(out, ':');
    if (c) { *c = '\0'; c = strrchr(out, ':'); if (c) *c = '\0'; }
}

static void resolve_vod(op_t *p);
static void resolve_fetch_movie(op_t *p, char t, const char *idbuf);

/* The portal's answer to "what are this series' seasons" while resolving. */
static void on_resolve_series(int success, int status, const char *body, size_t len, void *ud)
{
    (void)len;
    op_t *p = (op_t *)ud;
    char idbuf[EVO_PROVIDER_MAX_ITEM_ID];
    episode_series_id(p->arg, idbuf, sizeof idbuf);

    cJSON *root = NULL, *data = NULL;
    if (success && status >= 200 && status < 300) {
        cJSON *js = parse_js(body, &root);
        if (js && cJSON_IsObject(js))     data = cJSON_GetObjectItem(js, "data");
        else if (js && cJSON_IsArray(js)) data = js;
    }
    vc_t *e = vc_find(p->arg[0], idbuf);
    if (e && data && cJSON_IsArray(data) && cJSON_GetArraySize(data) > 0) {
        e->fetched = 1;
        vc_from_movie(e, data);
        
        PROV_LOG("stalker: series read again for resume, %d season(s)", e->nseasons);
    } else {
        PROV_LOG("stalker: series request for resume http=%d gave no list", status);
    }
    if (root) cJSON_Delete(root);
    resolve_vod(p);                                   /* refetched is set: no second round */
}

/* A film's own row, asked for by its id: its cmd. */
static void on_resolve_movie(int success, int status, const char *body, size_t len, void *ud)
{
    (void)len;
    op_t *p = (op_t *)ud;
    char idbuf[EVO_PROVIDER_MAX_ITEM_ID];
    snprintf(idbuf, sizeof idbuf, "%s", p->arg + 3);
    cJSON *root = NULL, *data = NULL;
    if (success && status >= 200 && status < 300) {
        cJSON *js = parse_js(body, &root);
        if (js && cJSON_IsObject(js))     data = cJSON_GetObjectItem(js, "data");
        else if (js && cJSON_IsArray(js)) data = js;
    }
    char cmd[384] = "";
    const cJSON *o;
    if (data && cJSON_IsArray(data)) {
        /* Only the film itself - never another row's link (a wrong film). */
        const int rows = cJSON_GetArraySize(data);
        cJSON_ArrayForEach(o, data) {
            char id[48];
            json_str(o, "id", id, sizeof id);
            if (strcmp(id, idbuf) == 0 || (rows == 1 && !id[0])) {
                json_str(o, "cmd", cmd, sizeof cmd);
                break;
            }
        }
    }
    if (cmd[0]) {
        vc_put(p->arg[0], idbuf, cmd, NULL, 0, "");
        cmd_store(p->arg[0], idbuf, cmd);
        PROV_LOG("stalker: film link asked again: found");
    } else {
        PROV_LOG("stalker: film link asked again: http=%d, %d row(s), none with this id", status,
                 (data && cJSON_IsArray(data)) ? cJSON_GetArraySize(data) : -1);
    }
    if (root) cJSON_Delete(root);
    resolve_vod(p);                                   /* refetched is set: no second round */
}

static void resolve_fetch_movie(op_t *p, char t, const char *idbuf)
{
    char eid[96], q[300];
    if (evo_provider_url_escape(idbuf, eid, sizeof eid) < 0) { resolve_vod(p); return; }
    snprintf(q, sizeof q, "type=%s&action=get_ordered_list&movie_id=%s&p=1",
             t == 'v' ? "vod" : "series", eid);
    if (api_get(q, on_resolve_movie, p, 1) != 0) resolve_vod(p);
}

/* The portal searched for the film's name: the row with its id has its link. */
/* The film's name made searchable: "NL The Family Plan 2 4K - 2025" or
 * "The Family Plan 2 (2025)" -> "The Family Plan 2". */
static void search_name(const char *title, char *out, size_t sz)
{
    char t[160];
    snprintf(t, sizeof t, "%s", title ? title : "");
    char *cut = strstr(t, " - ");
    if (cut) *cut = '\0';
    cut = strchr(t, '(');
    if (cut && cut > t) *cut = '\0';
    size_t n = strlen(t);
    while (n && (t[n - 1] == ' ' || t[n - 1] == '.')) t[--n] = '\0';
    /* Trailing quality labels ("4K", "HD", "1080p") and a leading language tag. */
    for (int again = 1; again; ) {
        again = 0;
        char *sp = strrchr(t, ' ');
        if (sp && (!strcmp(sp + 1, "4K") || !strcmp(sp + 1, "HD") || !strcmp(sp + 1, "FHD") ||
                   !strcmp(sp + 1, "UHD") || !strcmp(sp + 1, "1080p") || !strcmp(sp + 1, "720p"))) {
            *sp = '\0';
            again = 1;
        }
    }
    const char *s = t;
    if (s[0] == '|') { const char *e = strchr(s + 1, '|'); if (e) s = e + 1; }
    else if (s[0] >= 'A' && s[0] <= 'Z' && s[1] >= 'A' && s[1] <= 'Z' && s[2] == ' ' && s[3])
        s += 3;                                                          /* "NL ", "EN " */
    while (*s == ' ') s++;
    snprintf(out, sz, "%s", *s ? s : t);
}

static void resolve_search_page(op_t *p);

static void on_resolve_search(int success, int status, const char *body, size_t len, void *ud)
{
    (void)len;
    op_t *p = (op_t *)ud;
    char idbuf[EVO_PROVIDER_MAX_ITEM_ID];
    snprintf(idbuf, sizeof idbuf, "%s", p->arg + 3);
    cJSON *root = NULL, *data = NULL, *js = NULL;
    if (success && status >= 200 && status < 300) {
        js = parse_js(body, &root);
        if (js && cJSON_IsObject(js)) data = cJSON_GetObjectItem(js, "data");
    }
    char cmd[384] = "";
    int more = 0;
    const cJSON *o;
    if (data && cJSON_IsArray(data)) {
        const char *want = thint_for(p->arg);
        char wname[160] = "";
        if (want) search_name(want, wname, sizeof wname);
        cJSON_ArrayForEach(o, data) {
            char id[48], name[160];
            json_str(o, "id", id, sizeof id);
            if (strcmp(id, idbuf) == 0) { json_str(o, "cmd", cmd, sizeof cmd); break; }
            json_str(o, "name", name, sizeof name);
            /* The portal may have put the film back under a new id: the same
             * name, as long as only one row has it. */
            char nname[160];
            search_name(name, nname, sizeof nname);
            if (wname[0] && evo_tmdb_same_title(nname, "", wname, "", 0)) {
                if (!p->same_n) json_str(o, "cmd", p->same_cmd, sizeof p->same_cmd);
                p->same_n++;
            }
        }
        /* Not on this page: the next ones (a common name fills several). */
        const int total = json_int(js, "total_items"), per = json_int(js, "max_page_items");
        more = !cmd[0] && per > 0 && total > (p->page + 1) * per && p->page < 5 &&
               cJSON_GetArraySize(data) > 0;
    }
    if (root) cJSON_Delete(root);
    if (more) {
        p->page++;
        resolve_search_page(p);
        return;
    }
    if (!cmd[0] && p->same_n == 1 && p->same_cmd[0]) {
        snprintf(cmd, sizeof cmd, "%s", p->same_cmd);
        PROV_LOG("stalker: film by name: found under another id");
    }
    if (cmd[0] && strcmp(cmd, p->cmd) != 0) {
        vc_put(p->arg[0], idbuf, cmd, NULL, 0, "");
        cmd_store(p->arg[0], idbuf, cmd);
        PROV_LOG("stalker: film found by name - a new link");
        p->cached_cmd = 0;
        p->live = 0;
        p->page = 0;
        start_link(p, "vod", cmd, "");
        return;
    }
    PROV_LOG("stalker: film by name: http=%d, %d page(s), %d with that name, %s", status, p->page + 1,
             p->same_n, cmd[0] ? "the same link" : "not found");
    p->page = 0;
    if (!p->cmd[0]) {                                 /* never tried: the last guess */
        char guess[96];
        snprintf(guess, sizeof guess, "/media/file_%s.mpg", idbuf);
        start_link(p, "vod", guess, "");
        return;
    }
    op_fail(p);
}

static void resolve_search_page(op_t *p)
{
    const char *title = thint_for(p->arg);
    char name[160], esc[400], q[600];
    if (!title) { op_fail(p); return; }
    search_name(title, name, sizeof name);
    if (evo_provider_url_escape(name, esc, sizeof esc) < 0) { op_fail(p); return; }
    if (p->page == 0) PROV_LOG("stalker: looking the film up by name: '%s'", name);
    snprintf(q, sizeof q, "type=%s&action=get_ordered_list&search=%s&p=%d&sortby=added",
             p->arg[0] == 's' ? "series" : "vod", esc, p->page + 1);
    if (api_get(q, on_resolve_search, p, 1) != 0) op_fail(p);
}

static void resolve_search_movie(op_t *p)
{
    p->page = 0;
    p->same_n = 0;
    p->same_cmd[0] = '\0';
    resolve_search_page(p);
}

/* Resumed from Home / Recent: ask the portal for the series again. */
static void resolve_fetch_series(op_t *p, char t, const char *idbuf)
{
    if (!vc_find(t, idbuf)) vc_put(t, idbuf, "", NULL, 0, "");
    vc_t *e = vc_find(t, idbuf);
    char eid[96], ecat[48], q[420];
    if (!e || evo_provider_url_escape(idbuf, eid, sizeof eid) < 0 ||
        evo_provider_url_escape(e->cat, ecat, sizeof ecat) < 0) {
        op_fail(p);
        return;
    }
    snprintf(q, sizeof q,
             "type=%s&action=get_ordered_list&movie_id=%s&season_id=0&episode_id=0"
             "&category=%s&fav=0&p=1&sortby=added",
             t == 'v' ? "vod" : "series", eid, ecat);
    PROV_LOG("stalker: the series is not in memory - asking the portal for it");
    if (api_get(q, on_resolve_series, p, 1) != 0) op_fail(p);
}

/* The poster of the movie or series an item belongs to, "" when unknown. */
static void item_art_url(const char *arg, char *out, size_t sz)
{
    out[0] = '\0';
    if (!is_vod_id(arg, "me")) return;
    char idbuf[EVO_PROVIDER_MAX_ITEM_ID];
    if (arg[1] == 'e') episode_series_id(arg, idbuf, sizeof idbuf);
    else               snprintf(idbuf, sizeof idbuf, "%s", arg + 3);
    vc_t *e = vc_find(arg[0], idbuf);
    if (e && e->art[0]) snprintf(out, sz, "%s", e->art);
}

/* The year, the description and, for an episode, how many its season has. */
static void item_info(const char *arg, evo_stream_choice_t *c)
{
    if (!is_vod_id(arg, "me")) return;
    char idbuf[EVO_PROVIDER_MAX_ITEM_ID];
    if (arg[1] == 'e') episode_series_id(arg, idbuf, sizeof idbuf);
    else               snprintf(idbuf, sizeof idbuf, "%s", arg + 3);
    vc_t *e = vc_find(arg[0], idbuf);
    if (!e) return;
    snprintf(c->year, sizeof c->year, "%s", e->year);
    snprintf(c->overview, sizeof c->overview, "%s", e->desc);
    if (arg[1] == 'e' && e->nseasons > 0) {
        /* "se:<id>:<season index>:<episode>"; a flat series has index -1. */
        const char *c1 = strrchr(arg, ':');
        const char *c2 = NULL;
        for (const char *q = arg + 3; q < c1; ++q) if (*q == ':') c2 = q;
        int si = c2 ? atoi(c2 + 1) : 0;
        if (si < 0 || !e->has_seasons) si = 0;
        if (si < e->nseasons) c->ep_total = e->seasons[si].n;
    }
}

/* "vm:<id>" a movie, "ve:<id>:<season>:<episode>" / "se:..." an episode. */
static void resolve_vod(op_t *p)
{
    char t = p->arg[0], kind = p->arg[1];
    char idbuf[EVO_PROVIDER_MAX_ITEM_ID];
    snprintf(idbuf, sizeof idbuf, "%s", p->arg + 3);

    char cmd[384] = {0}, series[16] = {0};
    p->live = 0;

    if (kind == 'm') {
        vc_t *e = vc_find(t, idbuf);
        if (e && e->cmd[0]) {
            snprintf(cmd, sizeof cmd, "%s", e->cmd);
            p->cached_cmd = !p->refetched;
        } else if (cmd_lookup(t, idbuf, cmd, sizeof cmd)) {
            PROV_LOG("stalker: film link from the kept list");
            p->cached_cmd = 1;
        } else if (!p->refetched) {
            /* Not seen since a restart: ask the portal for the film itself. */
            p->refetched = 1;
            resolve_fetch_movie(p, t, idbuf);
            return;
        } else if (!p->searched && thint_for(p->arg)) {
            /* movie_id did not give it either: search for its name. */
            p->searched = 1;
            p->cmd[0] = '\0';
            resolve_search_movie(p);
            return;
        } else {
            snprintf(cmd, sizeof cmd, "/media/file_%s.mpg", idbuf);   /* the last guess */
        }
    } else {
        char *c = strrchr(idbuf, ':');
        if (!c) { op_fail(p); return; }
        snprintf(series, sizeof series, "%s", c + 1);
        *c = '\0';
        c = strrchr(idbuf, ':');
        if (!c) { op_fail(p); return; }
        int si = atoi(c + 1);
        *c = '\0';

        vc_t *e = vc_find(t, idbuf);
        if (!p->refetched && (!e || (e->nseasons <= 0 && !e->cmd[0]))) {
            /* Resumed from Home / Recent: the list that showed this series is
             * long gone, so ask the portal for it again, once, then go on. */
            p->refetched = 1;
            resolve_fetch_series(p, t, idbuf);
            return;
        }
        if (!e) {
            PROV_LOG("stalker: the series could not be found on the portal");
            op_fail(p);
            return;
        }
        snprintf(cmd, sizeof cmd, "%s", e->cmd);
        if (e->has_seasons && si >= 0 && si < e->nseasons && e->seasons[si].cmd[0])
            snprintf(cmd, sizeof cmd, "%s", e->seasons[si].cmd);
        if (e->nseasons > 0 && e->seasons) {
            int sidx = (e->has_seasons && si >= 0 && si < e->nseasons) ? si : 0;
            const vseason_t *sn = &e->seasons[sidx];
            for (int i = 0; i < sn->n; ++i)
                if (sn->num[i] == atoi(series) && sn->ecmd && sn->ecmd[i]) {
                    snprintf(cmd, sizeof cmd, "%s", sn->ecmd[i]);
                    series[0] = '\0';               /* the episode's own cmd already says which */
                    break;
                }
        }
    }
    if (!cmd[0] && kind == 'e' && !p->refetched) {
        /* What is in memory came from a list row with no cmd (or from an older
         * session): read the series from the portal once more, then decide. */
        p->refetched = 1;
        char sid[EVO_PROVIDER_MAX_ITEM_ID];
        episode_series_id(p->arg, sid, sizeof sid);
        resolve_fetch_series(p, t, sid);
        return;
    }
    if (!cmd[0] && kind == 'e' && t == 'v') {
        /* A film-style item with episodes: the portal's own file path. */
        char sid[EVO_PROVIDER_MAX_ITEM_ID];
        episode_series_id(p->arg, sid, sizeof sid);
        snprintf(cmd, sizeof cmd, "/media/file_%s.mpg", sid);
    }
    if (!cmd[0]) {
        PROV_LOG("stalker: this %s has no cmd to play", kind == 'e' ? "episode" : "film");
        op_fail(p);
        return;
    }
    PROV_LOG("stalker: playing a %s%s", kind == 'e' ? "series episode" : "film",
             series[0] ? " (by episode number)" : "");
    start_link(p, "vod", cmd, series);
}

static void run_resolve(op_t *p)
{
    if (is_vod_id(p->arg, "me")) { resolve_vod(p); return; }
    if (strncmp(p->arg, "ap:", 3) == 0) { resolve_archive(p); return; }
    if (strncmp(p->arg, "ci:", 3) == 0) {             /* search's "channels still loading" row */
        toast("SEARCH", "The channels are still loading - search again in a moment");
        op_fail(p);
        return;
    }
    if (strncmp(p->arg, "gi:", 3) == 0) {             /* a TV guide line: not playable */
        const char *ts = strchr(p->arg + 3, ':');
        const long a = ts ? atol(ts + 1) : 0;
        char when[64] = "That programme is not on now";
        if (strstr(p->arg, ":later") && a > 0) {
            time_t t = (time_t)a;
            struct tm tmv;
            localtime_r(&t, &tmv);
            snprintf(when, sizeof when, "Starts %02d-%02d at %02d:%02d", tmv.tm_mday, tmv.tm_mon + 1,
                     tmv.tm_hour, tmv.tm_min);
        } else if (strstr(p->arg, ":past")) {
            snprintf(when, sizeof when, "Already shown - not in the archive");
        }
        toast("TV GUIDE", when);
        op_fail(p);
        return;
    }
    if (strncmp(p->arg, "ch:", 3) != 0) { op_fail(p); return; }
    if (g_ch_n == 0) {                                /* e.g. resumed from Recent */
        if (api_get("type=itv&action=get_all_channels", on_channels, p, 1) != 0)
            op_fail(p);
        return;
    }
    do_resolve(p);
}

/* ---- dispatch ---------------------------------------------------------- */

static void op_run(op_t *p)
{
    if (!g_authed) {
        if (g_auth_busy) {                            /* a sign-in is on its way: wait for it */
            if (g_auth_wait_n < AUTH_WAIT_MAX) g_auth_wait[g_auth_wait_n++] = p;
            else op_fail(p);
            return;
        }
        g_auth_busy = 1;
        if (stalker_auth(on_op_auth, p) == -2) {      /* no memory: no callback either */
            g_auth_busy = 0;
            op_fail(p);
        }
        return;
    }
    if (g_ch_n == 0 && ch_load_disk() && g_ch_disk_age > CH_FRESH) {
        /* An old kept list: shown now, a new one asked for behind the scenes. */
        op_t *pl = op_new(OP_PRELOAD, "", 0);
        if (pl) op_run(pl);
    }
    if (p->op == OP_CATALOG)     run_catalog(p);
    else if (p->op == OP_SEARCH) run_search(p);
    else if (p->op == OP_PRELOAD) {
        /* Nothing to do when a list is in memory, unless it is a kept one
         * that is getting old: then the portal sends a new one meanwhile. */
        if (g_ch_loading || (g_ch_n > 0 && !(g_ch_from_disk && g_ch_disk_age > CH_FRESH))) { free(p); return; }
        g_ch_disk_age = 0;
        g_ch_loading = 1;
        PROV_LOG("stalker: loading the channel list for search, behind the scenes");
        if (api_get("type=itv&action=get_all_channels", on_channels, p, 1) != 0) {
            g_ch_loading = 0;
            free(p);
        }
    }
    else                         run_resolve(p);
}

static op_t *op_new(int op, const char *arg, int page)
{
    op_t *p = (op_t *)calloc(1, sizeof *p);
    if (!p) return NULL;
    p->op = op;
    p->page = page < 0 ? 0 : page;
    if (arg) snprintf(p->arg, sizeof p->arg, "%s", arg);
    return p;
}

static void preload_channels(void)
{
    op_t *pl = op_new(OP_PRELOAD, "", 0);
    if (pl) op_run(pl);
}

/* ------------------------------------------------------------------------- */
/* Several portals: ids leave here as "<portal>|<id>"                        */
/* ------------------------------------------------------------------------- */

typedef struct {
    evo_provider_items_cb cb;
    void *ud;
    int   k;
} pw_t;

static void pw_items(int ok, const evo_provider_item_t *items, int n, int more, void *ud)
{
    pw_t *w = (pw_t *)ud;
    evo_provider_item_t *it = (evo_provider_item_t *)items;   /* ours: stack or heap of the emitter */
    for (int i = 0; ok && it && i < n; ++i) {
        char tmp[EVO_PROVIDER_MAX_ITEM_ID];
        snprintf(tmp, sizeof tmp, "%d|%s", w->k, it[i].id);
        snprintf(it[i].id, sizeof it[i].id, "%s", tmp);
        if (it[i].parent_id[0]) {
            snprintf(tmp, sizeof tmp, "%d|%s", w->k, it[i].parent_id);
            snprintf(it[i].parent_id, sizeof it[i].parent_id, "%s", tmp);
        }
    }
    if (w->cb) w->cb(ok, items, n, more, w->ud);
    free(w);
}

static pw_t *pw_new(evo_provider_items_cb cb, void *ud, int k)
{
    pw_t *w = (pw_t *)calloc(1, sizeof *w);
    if (!w) return NULL;
    w->cb = cb;
    w->ud = ud;
    w->k = k;
    return w;
}

/* "2|g:5" -> portal 2, "g:5". An id without a number is portal 0 (older saved ids). */
static const char *split_portal(const char *id, int *k)
{
    *k = 0;
    if (!id) return "";
    const char *b = strchr(id, '|');
    if (!b || b == id) return id;
    for (const char *c = id; c < b; ++c)
        if (!isdigit((unsigned char)*c)) return id;
    *k = atoi(id);
    return b + 1;
}

static void emit_portals(evo_provider_items_cb cb, void *ud)
{
    evo_provider_item_t items[MAX_PORTALS];
    for (int i = 0; i < g_np; ++i) {
        evo_provider_item_clear(&items[i]);
        snprintf(items[i].id, sizeof items[i].id, "%d|", i);
        snprintf(items[i].title, sizeof items[i].title, "%s", g_portals[i].name);
        state_load();
        if (i < MAX_PORTALS && g_expiry[i][0])
            snprintf(items[i].subtitle, sizeof items[i].subtitle, "Subscription until %s", g_expiry[i]);
        else
            snprintf(items[i].subtitle, sizeof items[i].subtitle, "Portal");
        items[i].kind = EVO_MEDIA_FOLDER;
        items[i].is_folder = 1;
    }
    if (cb) cb(1, items, g_np, 0, ud);
}

static void portal_root(int k, evo_provider_items_cb cb, void *ud)
{
    pw_t *w = pw_new(cb, ud, k);
    if (!w) { if (cb) cb(0, NULL, 0, 0, ud); return; }
    emit_root(pw_items, w);
}

/* ------------------------------------------------------------------------- */
/* Searching a portal that is not the active one (Home's versions)           */
/* ------------------------------------------------------------------------- */
/*
 * Home looks every TMDB title up in every portal. Making another portal the
 * active one would sign the open one out and drop its lists, so the others
 * get a small session of their own: handshake, profile, search - nothing else.
 */
typedef struct {
    char ep[400];
    char token[96];
    int  ep_idx;
    long at;
} side_session_t;
static side_session_t g_sides[MAX_PORTALS];   /* one per portal, so two searches never mix */

typedef struct {
    int   k;
    char  type;                       /* 'v' films, 's' series */
    char  text[200];
    int   page;
    evo_provider_item_t *acc;
    int   n;
    pw_t *w;
} side_t;

static int side_get(int k, const char *query, evo_net_cb cb, void *ctx, int with_auth)
{
    side_session_t *ss = &g_sides[k];
    char url[EVO_NET_MAX_URL], ua[300], cookie[120], auth[160];
    const char *h[3];
    int nh = 0;
    int w = snprintf(url, sizeof url, "%s?%s&JsHttpRequest=1-xml", ss->ep, query);
    if (w < 0 || (size_t)w >= sizeof url) return -13;
    snprintf(ua, sizeof ua, "User-Agent: %s", STB_UA);
    snprintf(cookie, sizeof cookie, "Cookie: mac=%s; stb_lang=en; timezone=Europe/Amsterdam",
             g_portals[k].mac);
    h[nh++] = ua;
    h[nh++] = cookie;
    if (with_auth && ss->token[0]) {
        snprintf(auth, sizeof auth, "Authorization: Bearer %s", ss->token);
        h[nh++] = auth;
    }
    return evo_net_request_async("GET", url, NULL, h, nh, cb, ctx);
}

static void side_done(side_t *c, int ok)
{
    pw_t *w = c->w;
    evo_provider_item_t *acc = c->acc;
    const int n = c->n;
    free(c);
    pw_items(ok, acc, ok ? n : 0, 0, w);          /* frees w */
    free(acc);
}

static void side_search(side_t *c);

static void on_side_search(int success, int status, const char *body, size_t len, void *ud)
{
    (void)len;
    side_t *c = (side_t *)ud;
    if (!success || status < 200 || status >= 300) {
        g_sides[c->k].token[0] = '\0';                    /* sign in again next time */
        side_done(c, c->n > 0);
        return;
    }
    cJSON *root = NULL;
    cJSON *js = parse_js(body, &root);
    cJSON *data = (js && cJSON_IsObject(js)) ? cJSON_GetObjectItem(js, "data") : NULL;
    int more = 0;
    if (!data || !cJSON_IsArray(data)) {
        if (root) cJSON_Delete(root);
        g_sides[c->k].token[0] = '\0';
        side_done(c, c->n > 0);
        return;
    }
    const cJSON *o;
    g_cmd_portal = g_portals[c->k].url;            /* keep the links for this portal */
    cJSON_ArrayForEach(o, data) {
        if (c->n >= EVO_PROVIDER_PAGE_MAX) break;
        char id[48], cmd[384], name[EVO_PROVIDER_MAX_TITLE], year[16], art[EVO_PROVIDER_MAX_ART_URL];
        char cat[32], ck[40];
        json_str(o, "id", id, sizeof id);
        if (!id[0]) continue;
        json_str(o, "cmd", cmd, sizeof cmd);
        json_str(o, "name", name, sizeof name);
        evo_title_normalize_spaces(name);
        json_str(o, "year", year, sizeof year);
        json_str(o, "screenshot_uri", art, sizeof art);
        json_str(o, "category_id", cat, sizeof cat);
        snprintf(ck, sizeof ck, "%cc:%s", c->type, cat);
        if (evo_parental_hides("stalker", cat[0] ? ck : "", name)) continue;
        cJSON *sr = cJSON_GetObjectItem((cJSON *)o, "series");
        const int is_series = c->type == 's' || (sr && cJSON_IsArray(sr) && cJSON_GetArraySize(sr) > 0) ||
                              json_int(o, "is_series") == 1;
        if (!is_series) cmd_store(c->type, id, cmd);
        evo_provider_item_t *it = &c->acc[c->n++];
        evo_provider_item_clear(it);
        snprintf(it->id, sizeof it->id, "%c%c:%s", c->type, is_series ? 's' : 'm', id);
        snprintf(it->parent_id, sizeof it->parent_id, "search");
        snprintf(it->title, sizeof it->title, "%s", name[0] ? name : "Untitled");
        snprintf(it->subtitle, sizeof it->subtitle, "%.4s", year);
        if (strncmp(art, "http", 4) == 0) snprintf(it->art_url, sizeof it->art_url, "%s", art);
        it->kind = is_series ? EVO_MEDIA_FOLDER : EVO_MEDIA_VIDEO;
        it->is_folder = is_series ? 1 : 0;
    }
    g_cmd_portal = NULL;
    {
        const int total = json_int(js, "total_items"), per = json_int(js, "max_page_items");
        more = per > 0 && total > (c->page + 1) * per && c->page < 3 && cJSON_GetArraySize(data) > 0;
    }
    if (root) cJSON_Delete(root);
    if (more) { c->page++; side_search(c); return; }
    side_done(c, 1);
}

static void side_search(side_t *c)
{
    char esc[480], q[640];
    if (evo_provider_url_escape(c->text, esc, sizeof esc) < 0) { side_done(c, 0); return; }
    snprintf(q, sizeof q, "type=%s&action=get_ordered_list&search=%s&p=%d&sortby=added",
             c->type == 'v' ? "vod" : "series", esc, c->page + 1);
    if (side_get(c->k, q, on_side_search, c, 1) != 0) side_done(c, 0);
}

static void side_handshake(side_t *c);

static void on_side_profile(int success, int status, const char *body, size_t len, void *ud)
{
    (void)body; (void)len;
    side_t *c = (side_t *)ud;
    if (!success || status < 200 || status >= 300) {
        PROV_LOG("stalker: portal %d (for Home's search) refused the profile, http=%d", c->k + 1, status);
        g_sides[c->k].token[0] = '\0';
        side_done(c, 0);
        return;
    }
    g_sides[c->k].at = (long)time(NULL);
    PROV_LOG("stalker: portal %d signed in for Home's search", c->k + 1);
    side_search(c);
}

static void on_side_handshake(int success, int status, const char *body, size_t len, void *ud)
{
    (void)len;
    side_t *c = (side_t *)ud;
    char token[sizeof g_sides[0].token] = "";
    if (success && status >= 200 && status < 300) {
        cJSON *root = NULL;
        cJSON *js = parse_js(body, &root);
        if (js) json_str(js, "token", token, sizeof token);
        if (root) cJSON_Delete(root);
    }
    if (!token[0]) {
        g_sides[c->k].ep_idx++;
        side_handshake(c);
        return;
    }
    snprintf(g_sides[c->k].token, sizeof g_sides[c->k].token, "%s", token);
    if (side_get(c->k, "type=stb&action=get_profile&hd=1&num_banks=2&stb_type=MAG250"
                 "&client_type=STB&image_version=218&video_out=hdmi"
                 "&auth_second_step=0&hw_version=1.7-BD-00&not_valid_token=0"
                 "&api_signature=262", on_side_profile, c, 1) != 0)
        side_done(c, 0);
}

static void side_handshake(side_t *c)
{
    g_sides[c->k].token[0] = '\0';
    if (endpoint_for_url(g_portals[c->k].url, g_sides[c->k].ep_idx, g_sides[c->k].ep, sizeof g_sides[c->k].ep) != 0) {
        PROV_LOG("stalker: portal %d did not answer Home's search", c->k + 1);
        g_sides[c->k].ep_idx = 0;
        side_done(c, 0);
        return;
    }
    if (side_get(c->k, "type=stb&action=handshake&token=&prehash=0", on_side_handshake, c, 0) != 0)
        side_done(c, 0);
}

/* "@findin:<k>:v:<text>" for a portal other than the active one. */
static int side_find(int k, char type, const char *text, evo_provider_items_cb cb, void *ud)
{
    side_t *c = (side_t *)calloc(1, sizeof *c);
    if (!c) { if (cb) cb(0, NULL, 0, 0, ud); return -2; }
    c->acc = (evo_provider_item_t *)calloc(EVO_PROVIDER_PAGE_MAX, sizeof *c->acc);
    c->w = pw_new(cb, ud, k);
    if (!c->acc || !c->w) {
        free(c->acc); free(c->w); free(c);
        if (cb) cb(0, NULL, 0, 0, ud);
        return -2;
    }
    c->k = k;
    c->type = type;
    snprintf(c->text, sizeof c->text, "%s", text);
    const long now = (long)time(NULL);
    if (g_sides[k].token[0] && now - g_sides[k].at < 20 * 60) {
        side_search(c);
        return 0;
    }
    g_sides[k].ep_idx = 0;
    side_handshake(c);
    return 0;
}

/* Settings > Clear cache: the lists kept on disk, and the channel list in memory. */
void stalker_clear_cache(void)
{
    sc_clear_all();
    PROV_LOG("stalker: cache cleared");
}

int stalker_portal_count(void)
{
    return g_np;
}

static int stalker_list_catalog(const char *parent_id, int page,
                                evo_provider_items_cb cb, void *ud)
{
    PROV_LOG("stalker: list_catalog parent='%s' page=%d portals=%d",
             parent_id ? parent_id : "", page, g_np);
    if (g_np == 0) {
        if (cb) cb(0, NULL, 0, 0, ud);
        return -1;
    }
    if (!parent_id || !*parent_id) {                  /* the top: no request needed */
        if (g_np == 1) portal_root(0, cb, ud);
        else           emit_portals(cb, ud);
        return 0;
    }

    int k;
    const char *inner;
    char newest[8];
    char findbuf[EVO_PROVIDER_MAX_ITEM_ID];
    if (strncmp(parent_id, "@findin:", 8) == 0) {
        /* "@findin:<k>:v:<text>": Home's lookup in one portal. The active one
         * (or none yet) searches as usual; another gets a session of its own. */
        const int fk = atoi(parent_id + 8);
        const char *t = strchr(parent_id + 8, ':');
        if (fk < 0 || fk >= g_np || !t || (t[1] != 'v' && t[1] != 's') || t[2] != ':') {
            if (cb) cb(0, NULL, 0, 0, ud);
            return -1;
        }
        if (g_active >= 0 && g_active != fk) {
            /* The same portal and MAC twice: a second sign-in would end the
             * first one's session. What the active one finds covers it. */
            if (strcmp(g_portals[fk].url, g_portals[g_active].url) == 0 &&
                strcasecmp(g_portals[fk].mac, g_portals[g_active].mac) == 0) {
                if (cb) cb(1, NULL, 0, 0, ud);
                return 0;
            }
            return side_find(fk, t[1], t + 3, cb, ud);
        }
        if (g_active < 0) activate(fk);
        snprintf(findbuf, sizeof findbuf, EVO_PROVIDER_FIND "%c:%s", t[1], t + 3);
        parent_id = findbuf;
    }
    if (strncmp(parent_id, EVO_PROVIDER_FIND, strlen(EVO_PROVIDER_FIND)) == 0 &&
        (parent_id[6] == 'v' || parent_id[6] == 's') && parent_id[7] == ':') {
        /* "@find:v:<title>": films (or series) whose name has this in it. */
        k = (g_active >= 0 && g_active < g_np) ? g_active : 0;
        activate(k);
        pw_t *fw = pw_new(cb, ud, k);
        if (!fw) return -2;
        op_t *fp = op_new(OP_SEARCH, parent_id + 8, 0);
        if (!fp) { free(fw); return -2; }
        fp->only = parent_id[6] == 'v' ? 1 : 2;
        fp->icb = pw_items;
        fp->ud = fw;
        op_run(fp);
        return 0;
    }
    if (strncmp(parent_id, "@new:", 5) == 0) {
        /* Home's rows: the portal used last, no portal in the id. */
        k = (g_active >= 0 && g_active < g_np) ? g_active : 0;
        snprintf(newest, sizeof newest, "%cn:*", parent_id[5] == 's' ? 's' : 'v');
        inner = newest;
    } else {
        inner = split_portal(parent_id, &k);
    }
    if (k < 0 || k >= g_np) {
        if (cb) cb(0, NULL, 0, 0, ud);
        return -1;
    }
    if (!*inner) {                                    /* a portal's own folder */
        portal_root(k, cb, ud);
        return 0;
    }

    activate(k);
    pw_t *w = pw_new(cb, ud, k);
    if (!w) return -2;
    op_t *p = op_new(OP_CATALOG, inner, page);
    if (!p) { free(w); return -2; }
    p->icb = pw_items;
    p->ud = w;
    if (is_vod_id(inner, "sz") && series_ready(inner)) {   /* seasons / episodes: from cache */
        emit_series_children(p);
        return 0;
    }
    op_run(p);
    return 0;
}

/*
 * Search in every portal: the open one as before (channels, films, series),
 * then the films and series of each other portal through its search-only
 * session, so the open portal stays signed in. With more than one portal each
 * row says where it is.
 */
typedef struct {
    evo_provider_items_cb cb;
    void *ud;
    char  query[256];
    evo_provider_item_t *acc;
    int   n;
    int   step;                       /* see msearch_next */
    int   first;                      /* the open portal */
} msearch_t;

static void msearch_next(msearch_t *m);

static void portal_label(int k, char *out, size_t sz)
{
    if (g_portals[k].name[0]) snprintf(out, sz, "%s", g_portals[k].name);
    else                      snprintf(out, sz, "Portal %d", k + 1);
}

static void msearch_add(msearch_t *m, const evo_provider_item_t *items, int count)
{
    for (int i = 0; items && i < count && m->n < EVO_PROVIDER_PAGE_MAX; ++i) {
        int dup = 0;                                  /* a portal that repeats a row on its next page */
        for (int j = 0; j < m->n && !dup; ++j) dup = strcmp(m->acc[j].id, items[i].id) == 0;
        if (!dup) m->acc[m->n++] = items[i];
    }
}

static void on_msearch(int ok, const evo_provider_item_t *items, int count, int more, void *ud)
{
    (void)more;
    msearch_t *m = (msearch_t *)ud;
    if (ok) msearch_add(m, items, count);
    m->step++;
    msearch_next(m);
}

/* Which kind a row is: 0 film, 1 series, 2 live TV (and the loading row). */
static int row_group(const evo_provider_item_t *it)
{
    if (it->is_live || strstr(it->id, "|ch:") || strstr(it->id, "|ci:")) return 2;
    return it->is_folder ? 1 : 0;
}

/* Films first, then series, then live TV; each row says what it is and, with
 * more than one portal, where. */
static void msearch_finish(msearch_t *m)
{
    evo_provider_item_t *out = (evo_provider_item_t *)calloc((size_t)(m->n ? m->n : 1), sizeof *out);
    int n = 0, count[3] = {0, 0, 0};
    for (int g = 0; out && g < 3; ++g)
        for (int i = 0; i < m->n; ++i) {
            if (row_group(&m->acc[i]) != g) continue;
            evo_provider_item_t *it = &out[n++];
            *it = m->acc[i];
            count[g]++;
            if (strstr(it->id, "|ci:")) continue;
            static const char *const kind[3] = { "Film", "Series", "Live TV" };
            char label[48] = "", sub[sizeof it->subtitle];
            if (g_np > 1) {
                int k = 0;
                split_portal(it->id, &k);
                portal_label(k, label, sizeof label);
            }
            const char *old = (g == 2 && strcmp(it->subtitle, "Live Broadcast") == 0) ? "" : it->subtitle;
            snprintf(sub, sizeof sub, "%s%s%s%s%s", kind[g], old[0] ? "  -  " : "", old,
                     label[0] ? "  -  " : "", label);
            snprintf(it->subtitle, sizeof it->subtitle, "%s", sub);
        }
    PROV_LOG("stalker: search '%s': %d film(s), %d series, %d channel(s)", m->query, count[0], count[1], count[2]);
    if (m->cb) m->cb(1, out ? out : m->acc, out ? n : m->n, 0, m->ud);
    free(out);
    free(m->acc);
    free(m);
}

/* The open portal's channels, from the list in memory. When it is not loaded
 * yet (16000 channels take a while) it is fetched behind the scenes and one
 * row says so - films and series do not wait for it. */
static void msearch_channels(msearch_t *m)
{
    if (g_active != m->first) return;
    if (g_ch_n == 0) {
        op_t *pl = op_new(OP_PRELOAD, "", 0);
        if (pl) op_run(pl);
        if (m->n < EVO_PROVIDER_PAGE_MAX) {
            evo_provider_item_t *it = &m->acc[m->n++];
            evo_provider_item_clear(it);
            snprintf(it->id, sizeof it->id, "%d|ci:loading", m->first);
            snprintf(it->title, sizeof it->title, "Live TV channels are still loading");
            snprintf(it->subtitle, sizeof it->subtitle, "Search again in a moment to find channels too");
            it->kind = EVO_MEDIA_STREAM;
            it->is_live = 1;
        }
        return;
    }
    for (int i = 0; i < g_ch_n && m->n < EVO_PROVIDER_PAGE_MAX; ++i) {
        const stalker_ch_t *c = &g_ch[i];
        if (!ci_contains(c->name, m->query)) continue;
        char gk[40];
        snprintf(gk, sizeof gk, "g:%s", c->genre);
        if (evo_parental_hides("stalker", c->genre[0] ? gk : "", c->name)) continue;
        evo_provider_item_t *it = &m->acc[m->n++];
        evo_provider_item_clear(it);
        snprintf(it->id, sizeof it->id, "%d|ch:%s", m->first, c->id);
        snprintf(it->parent_id, sizeof it->parent_id, "%d|search", m->first);
        snprintf(it->title, sizeof it->title, "%s", c->name[0] ? c->name : "Channel");
        if (strncmp(c->logo, "http", 4) == 0) snprintf(it->art_url, sizeof it->art_url, "%s", c->logo);
        it->kind = EVO_MEDIA_STREAM;
        it->is_live = 1;
    }
}

/*
 * Search in every portal, quickly: the open portal's films (step 0) and series
 * (step 1), then each other portal's films and series through its search-only
 * session (two steps per portal), and last the open portal's channels from
 * memory. Nothing waits for the channel list.
 */
static void msearch_next(msearch_t *m)
{
    while (1) {
        if (m->step <= 1) {
            activate(m->first);
            pw_t *w = pw_new(on_msearch, m, m->first);
            op_t *p = w ? op_new(OP_SEARCH, m->query, 0) : NULL;
            if (!p) { free(w); m->step++; continue; }
            p->only = m->step == 0 ? 1 : 2;
            p->icb = pw_items;
            p->ud = w;
            op_run(p);
            return;
        }
        const int idx = (m->step - 2) / 2;
        int k = -1, seen = 0;
        for (int i = 0; i < g_np; ++i) {
            if (i == m->first) continue;
            if (strcmp(g_portals[i].url, g_portals[m->first].url) == 0 &&
                strcasecmp(g_portals[i].mac, g_portals[m->first].mac) == 0) continue;
            if (seen++ == idx) { k = i; break; }
        }
        if (k < 0) break;
        const char type = ((m->step - 2) % 2) ? 's' : 'v';
        side_find(k, type, m->query, on_msearch, m);
        return;
    }
    msearch_channels(m);
    msearch_finish(m);
}

static int stalker_search(const char *query, int page,
                          evo_provider_items_cb cb, void *ud)
{
    if (g_np == 0 || !query || !*query) {
        if (cb) cb(0, NULL, 0, 0, ud);
        return -1;
    }
    if (page > 0) {                                   /* one combined page */
        if (cb) cb(1, NULL, 0, 0, ud);
        return 0;
    }
    msearch_t *m = (msearch_t *)calloc(1, sizeof *m);
    if (!m) return -2;
    m->acc = (evo_provider_item_t *)calloc(EVO_PROVIDER_PAGE_MAX, sizeof *m->acc);
    if (!m->acc) { free(m); return -2; }
    m->cb = cb;
    m->ud = ud;
    snprintf(m->query, sizeof m->query, "%s", query);
    m->first = (g_active >= 0 && g_active < g_np) ? g_active : 0;
    msearch_next(m);
    return 0;
}

static int stalker_resolve(const char *item_id, evo_provider_resolve_cb cb, void *ud)
{
    if (!item_id || g_np == 0) return -1;
    int k;
    const char *inner = split_portal(item_id, &k);
    if (k < 0 || k >= g_np) return -1;
    activate(k);
    op_t *p = op_new(OP_RESOLVE, inner, 0);
    if (!p) return -2;
    p->rcb = cb;
    p->ud = ud;
    op_run(p);
    return 0;
}

/* ------------------------------------------------------------------------- */
/* Adding and removing portals, hiding and moving categories                 */
/* ------------------------------------------------------------------------- */

static const char *stalker_get_source(void)
{
    if (g_np == 0) return "";
    int k = (g_active >= 0 && g_active < g_np) ? g_active : 0;
    snprintf(g_source, sizeof g_source, "%s %s", g_portals[k].url, g_portals[k].mac);
    return g_source;
}

/* "<url> <mac>" adds a portal; "" removes them all. */
static int stalker_set_source(const char *value)
{
    if (!value || !*value) {
        sc_clear_all();               /* signed out of every portal: nothing to keep */
        g_np = 0;
        memset(g_portals, 0, sizeof g_portals);
        memset(g_prefs, 0, sizeof g_prefs);
        chord_free_all();
        chord_save();
        forget_active();
        return stalker_save_config();
    }

    char buf[320];
    snprintf(buf, sizeof buf, "%s", value);
    for (char *c = buf; *c; ++c)
        if (*c == ';' || *c == '|' || *c == ',' || *c == '\t') *c = ' ';
    trim(buf);

    char url[320] = {0}, mac[320] = {0};
    char *sp = strchr(buf, ' ');
    if (sp) {
        *sp = '\0';
        snprintf(mac, sizeof mac, "%s", sp + 1);
        trim(mac);
    }
    snprintf(url, sizeof url, "%s", buf);
    if (!url[0] || !mac[0] || url[0] == '/') return -1;   /* a file path is not a portal */

    char full[340];
    if (strncmp(url, "http://", 7) != 0 && strncmp(url, "https://", 8) != 0)
        snprintf(full, sizeof full, "http://%s", url);
    else
        snprintf(full, sizeof full, "%s", url);

    /* A bare host gets the usual /c/ path. */
    const char *after = strstr(full, "://") + 3;
    if (!strchr(after, '/')) strncat(full, "/c/", sizeof full - strlen(full) - 1);
    else if (full[strlen(full) - 1] != '/' && !strstr(full, ".php"))
        strncat(full, "/", sizeof full - strlen(full) - 1);

    char norm[24];
    if (mac_normalize(mac, norm, sizeof norm) != 0) return -1;

    for (int i = 0; i < g_np; ++i)                    /* already there: nothing to add */
        if (strcmp(g_portals[i].url, full) == 0 && strcmp(g_portals[i].mac, norm) == 0)
            return 0;
    if (g_np >= MAX_PORTALS) return -1;

    portal_t *pt = &g_portals[g_np];
    memset(pt, 0, sizeof *pt);
    snprintf(pt->url, sizeof pt->url, "%s", full);
    snprintf(pt->mac, sizeof pt->mac, "%s", norm);
    host_name(full, pt->name, sizeof pt->name);
    for (int i = 0; i < g_np; ++i)                    /* two portals on one host: tell them apart */
        if (strcmp(g_portals[i].name, pt->name) == 0) {
            char tail[8];
            snprintf(tail, sizeof tail, " %s", norm + 12);       /* the last two bytes */
            strncat(pt->name, tail, sizeof pt->name - strlen(pt->name) - 1);
            break;
        }
    clean_text(pt->name);
    memset(&g_prefs[g_np], 0, sizeof g_prefs[g_np]);
    g_np++;
    forget_active();
    return stalker_save_config();
}

static void stalker_sign_out(void)
{
    stalker_set_source("");
}

/*
 * Continue Watching and Favorites keep a Stalker item as
 * "evo://stalker/<portal number>|<id>". The numbers are positions in the portal
 * list, so removing portal 0 used to leave every saved item pointing at the
 * portal after it. Items of the removed portal go; the rest follow their portal.
 */
static int saved_key_portal(const char *path, const char **rest)
{
    static const char pre[] = "evo://stalker/";
    if (strncmp(path, pre, sizeof pre - 1) != 0) return -1;
    const char *n = path + sizeof pre - 1;
    const char *bar = strchr(n, '|');
    if (!bar || bar == n) return -1;
    for (const char *c = n; c < bar; ++c)
        if (!isdigit((unsigned char)*c)) return -1;
    *rest = bar;                                      /* "|vm:123" */
    return atoi(n);
}

static void renumber_saved_keys(int removed)
{
    int changed = 0;
    for (int i = recent_file_count - 1; i >= 0; --i) {
        const char *rest = NULL;
        const int j = saved_key_portal(recent_files[i].path, &rest);
        if (j < 0 || j < removed) continue;
        if (j == removed) {
            recent_remove_at(i);                      /* saves */
            continue;
        }
        char moved[sizeof recent_files[0].path];
        snprintf(moved, sizeof moved, "evo://stalker/%d%s", j - 1, rest);
        snprintf(recent_files[i].path, sizeof recent_files[i].path, "%s", moved);
        changed = 1;
    }
    if (changed) recent_save();

    changed = 0;
    for (int i = favorite_count - 1; i >= 0; --i) {
        const char *rest = NULL;
        const int j = saved_key_portal(favorite_files[i].path, &rest);
        if (j < 0 || j < removed) continue;
        if (j == removed) {
            char gone[sizeof favorite_files[0].path];
            snprintf(gone, sizeof gone, "%s", favorite_files[i].path);
            favorites_remove(gone);
            changed = 1;
            continue;
        }
        char moved[sizeof favorite_files[0].path];
        snprintf(moved, sizeof moved, "evo://stalker/%d%s", j - 1, rest);
        snprintf(favorite_files[i].path, sizeof favorite_files[i].path, "%s", moved);
        changed = 1;
    }
    if (changed) favorites_save();
}

static void remove_portal(int k)
{
    for (int i = k; i < g_np - 1; ++i) {
        g_portals[i] = g_portals[i + 1];
        g_prefs[i] = g_prefs[i + 1];
    }
    memset(&g_portals[g_np - 1], 0, sizeof g_portals[0]);
    memset(&g_prefs[g_np - 1], 0, sizeof g_prefs[0]);
    g_np--;
    for (int i = 0; i < g_nchord; ++i) {              /* the lists of the removed portal go, the rest shift */
        if (g_chord[i].portal == k) {
            free(g_chord[i].ids);
            g_chord[i] = g_chord[--g_nchord];
            memset(&g_chord[g_nchord], 0, sizeof g_chord[0]);
            i--;
        } else if (g_chord[i].portal > k) {
            g_chord[i].portal--;
        }
    }
    chord_save();
    renumber_saved_keys(k);
    state_renumber(k);
    forget_active();
    stalker_save_config();
}

static void say(char *msg, size_t sz, const char *fmt, const char *arg)
{
    if (msg && sz) snprintf(msg, sz, fmt, arg ? arg : "");
}

/* "g:5" -> live 5, "vc:7" -> movies 7, "sc:9" -> series 9. Not "g:*", not a section. */
static int parse_cat(const char *inner, int *scope, char *id, size_t sz)
{
    const char *rest;
    if (strncmp(inner, "g:", 2) == 0)       { *scope = SC_LIVE;   rest = inner + 2; }
    else if (strncmp(inner, "vc:", 3) == 0) { *scope = SC_VOD;    rest = inner + 3; }
    else if (strncmp(inner, "sc:", 3) == 0) { *scope = SC_SERIES; rest = inner + 3; }
    else return -1;
    if (!*rest || strcmp(rest, "*") == 0 || rest[0] == '@') return -1;
    snprintf(id, sz, "%s", rest);
    return 0;
}

int stalker_item_action(const char *item_id, int action, char *msg, size_t msg_sz)
{
    if (msg && msg_sz) msg[0] = '\0';
    if (!item_id || !strchr(item_id, '|')) return -1;

    int k;
    const char *inner = split_portal(item_id, &k);
    if (k < 0 || k >= g_np) return -1;

    if (!*inner) {                                    /* a portal's own folder */
        static int pending = -1;
        static time_t pending_at;
        if (action != STALKER_ACT_HIDE) {
            say(msg, msg_sz, "Portals keep the order they were added in%s", "");
            return 1;
        }
        time_t now = time(NULL);
        if (pending != k || now - pending_at > 6) {
            pending = k;
            pending_at = now;
            say(msg, msg_sz, "Press Triangle again to remove %s", g_portals[k].name);
            return 1;
        }
        pending = -1;
        remove_portal(k);
        say(msg, msg_sz, "Portal removed%s", "");
        return 0;
    }

    int scope;
    char cid[16];
    if (parse_cat(inner, &scope, cid, sizeof cid) != 0) return -1;
    prefs_t *pr = &g_prefs[k];

    if (action == STALKER_ACT_HIDE) {
        if (pref_has(pr->hid[scope], pr->nh[scope], cid) < 0) {
            if (pr->nh[scope] >= MAX_PREF) {
                say(msg, msg_sz, "Too many hidden categories - restore some first%s", "");
                return 1;
            }
            snprintf(pr->hid[scope][pr->nh[scope]++], 16, "%s", cid);
        }
        int at = pref_has(pr->ord[scope], pr->no[scope], cid);
        if (at >= 0) {
            for (int i = at; i < pr->no[scope] - 1; ++i)
                memcpy(pr->ord[scope][i], pr->ord[scope][i + 1], 16);
            pr->no[scope]--;
        }
        stalker_save_config();
        say(msg, msg_sz, "Category hidden - restore it from the setup page (Options)%s", "");
        return 0;
    }

    /* Move up / down: swap with the neighbour on screen, then keep the whole order. */
    int dir = (action == STALKER_ACT_UP) ? -1 : 1;
    int i = (g_vis_portal == k) ? pref_has((const char (*)[16])g_vis[scope], g_vis_n[scope], cid) : -1;
    if (i < 0) {
        say(msg, msg_sz, "Open the list again, then move it%s", "");
        return 1;
    }
    int j = i + dir;
    if (j < 0 || j >= g_vis_n[scope]) {
        say(msg, msg_sz, dir < 0 ? "Already at the top%s" : "Already at the bottom%s", "");
        return 1;
    }
    char tmp[16];
    memcpy(tmp, g_vis[scope][i], 16);
    memcpy(g_vis[scope][i], g_vis[scope][j], 16);
    memcpy(g_vis[scope][j], tmp, 16);
    pr->no[scope] = 0;
    for (int n = 0; n < g_vis_n[scope] && pr->no[scope] < MAX_ORD; ++n)
        memcpy(pr->ord[scope][pr->no[scope]++], g_vis[scope][n], 16);
    stalker_save_config();
    return 0;
}

int stalker_cycle_sort(const char *item_id, int dir, char *msg, size_t msg_sz)
{
    if (msg && msg_sz) msg[0] = '\0';
    if (!item_id || !strchr(item_id, '|')) return -1;
    int k;
    const char *inner = split_portal(item_id, &k);
    if (k < 0 || k >= g_np) return -1;

    int scope;
    if (strncmp(inner, "ch:", 3) == 0)        scope = SC_LIVE;
    else if (is_vod_id(inner, "mse"))         scope = (inner[0] == 'v') ? SC_VOD : SC_SERIES;
    else return -1;

    int n = sort_modes(scope);
    g_sort[scope] = ((g_sort[scope] + (dir < 0 ? -1 : 1)) % n + n) % n;
    stalker_save_config();
    const char *name = scope == SC_LIVE ? k_live_sort[g_sort[scope]] : k_vod_sort[g_sort[scope]];
    if (msg && msg_sz) snprintf(msg, msg_sz, "Sorted by: %s", name);
    return 0;
}

/* Moves element `from` of an array to place `to`, the ones between shift by one. */
static void move_elem(void *base, size_t esz, int from, int to)
{
    char tmp[32];
    char *b = (char *)base;
    if (esz > sizeof tmp || from == to) return;
    memcpy(tmp, b + (size_t)from * esz, esz);
    if (to > from) memmove(b + (size_t)from * esz, b + (size_t)(from + 1) * esz, (size_t)(to - from) * esz);
    else           memmove(b + (size_t)(to + 1) * esz, b + (size_t)to * esz, (size_t)(from - to) * esz);
    memcpy(b + (size_t)to * esz, tmp, esz);
}

int stalker_move_info(const char *item_id, int *pos, int *total)
{
    if (!item_id || !strchr(item_id, '|')) return -1;
    int k;
    const char *inner = split_portal(item_id, &k);
    if (k < 0 || k >= g_np || !*inner) return -1;

    if (strncmp(inner, "ch:", 3) == 0) {
        if (g_last_portal != k || g_active != k || g_ch_n <= 0) return -1;
        int *idx = (int *)malloc((size_t)g_ch_n * sizeof *idx);
        if (!idx) return -1;
        int m = channel_order(g_last_want, idx), at = -1;
        for (int j = 0; j < m && at < 0; ++j)
            if (strcmp(g_ch[idx[j]].id, inner + 3) == 0) at = j;
        free(idx);
        if (at < 0) return -1;
        *pos = at + 1;
        *total = m;
        return 0;
    }
    int scope;
    char cid[16];
    if (parse_cat(inner, &scope, cid, sizeof cid) != 0 || g_vis_portal != k) return -1;
    int i = pref_has((const char (*)[16])g_vis[scope], g_vis_n[scope], cid);
    if (i < 0) return -1;
    *pos = i + 1;
    *total = g_vis_n[scope];
    return 0;
}

int stalker_move_to(const char *item_id, int new_pos, char *msg, size_t msg_sz)
{
    if (msg && msg_sz) msg[0] = '\0';
    int pos, total;
    if (stalker_move_info(item_id, &pos, &total) != 0) {
        say(msg, msg_sz, "Pick a category or a channel first%s", "");
        return 1;
    }
    if (new_pos < 1) new_pos = 1;
    if (new_pos > total) new_pos = total;
    if (new_pos == pos) {
        if (msg && msg_sz) snprintf(msg, msg_sz, "Already at position %d", pos);
        return 1;
    }
    int k;
    const char *inner = split_portal(item_id, &k);

    if (strncmp(inner, "ch:", 3) == 0) {
        if (total > MAX_CH_ORDER) {
            say(msg, msg_sz, "This list is too long to put in your own order%s", "");
            return 1;
        }
        int *idx = (int *)malloc((size_t)g_ch_n * sizeof *idx);
        char (*ids)[24] = (char (*)[24])malloc((size_t)total * sizeof *ids);
        if (!idx || !ids) { free(idx); free(ids); return 1; }
        int m = channel_order(g_last_want, idx);
        for (int j = 0; j < m && j < total; ++j) snprintf(ids[j], 24, "%s", g_ch[idx[j]].id);
        move_elem(ids, 24, pos - 1, new_pos - 1);
        int rc = chord_set(k, g_last_want, ids, m < total ? m : total);
        free(idx);
        free(ids);
        if (rc != 0) { say(msg, msg_sz, "Too many lists with their own order%s", ""); return 1; }
        chord_save();
        g_sort[SC_LIVE] = 0;                          /* "Portal order" is where the moves show */
        stalker_save_config();
    } else {
        int scope;
        char cid[16];
        if (parse_cat(inner, &scope, cid, sizeof cid) != 0) return 1;
        prefs_t *pr = &g_prefs[k];
        move_elem(g_vis[scope], 16, pos - 1, new_pos - 1);
        pr->no[scope] = 0;
        for (int n = 0; n < g_vis_n[scope] && pr->no[scope] < MAX_ORD; ++n)
            memcpy(pr->ord[scope][pr->no[scope]++], g_vis[scope][n], 16);
        stalker_save_config();
    }
    if (msg && msg_sz) snprintf(msg, msg_sz, "Moved to position %d", new_pos);
    return 0;
}

/* The episode that is playing, again: a look-ahead resolve of the next one
 * (the last minute of an episode) overwrote it. */
void stalker_note_playing(const char *item_id, const char *url)
{
    if (!item_id || !url) return;
    const char *bar = strchr(item_id, '|');
    const char *inner = bar ? bar + 1 : item_id;
    if (!is_vod_id(inner, "e")) return;
    g_play_ep_portal = bar ? atoi(item_id) : g_active;
    snprintf(g_play_ep, sizeof g_play_ep, "%s", inner);
    snprintf(g_play_ep_url, sizeof g_play_ep_url, "%s", url);
}

int stalker_is_playing_episode(const char *url)
{
    return url && g_play_ep_url[0] && strcmp(url, g_play_ep_url) == 0;
}

/* The episode before / after the one playing: the same season, else the next / previous one. */
int stalker_episode_neighbor(int dir, char *item_id, size_t id_sz, char *title, size_t title_sz)
{
    if (!g_play_ep[0] || g_play_ep_portal < 0 || g_play_ep_portal >= g_np || g_active != g_play_ep_portal)
        return -1;
    const char t = g_play_ep[0];
    char idbuf[EVO_PROVIDER_MAX_ITEM_ID];
    snprintf(idbuf, sizeof idbuf, "%s", g_play_ep + 3);
    char *c = strrchr(idbuf, ':');
    if (!c) return -1;
    const int num = atoi(c + 1);
    *c = '\0';
    c = strrchr(idbuf, ':');
    if (!c) return -1;
    const int si = atoi(c + 1);
    *c = '\0';

    vc_t *e = vc_find(t, idbuf);
    if (!e || e->nseasons <= 0 || !e->seasons) return -1;
    const int sidx = (e->has_seasons && si >= 0 && si < e->nseasons) ? si : 0;
    const vseason_t *sn = &e->seasons[sidx];
    int at = -1;
    for (int i = 0; i < sn->n && at < 0; ++i)
        if (sn->num[i] == num) at = i;
    if (at < 0) return -1;

    int ns = sidx, ni = at + (dir < 0 ? -1 : 1);
    if (ni < 0) {                                     /* before the first: the last of the season before */
        if (!e->has_seasons || sidx == 0) return -1;
        ns = sidx - 1;
        if (e->seasons[ns].n <= 0) return -2;
        ni = e->seasons[ns].n - 1;
    } else if (ni >= sn->n) {                         /* after the last: the first of the next season */
        if (!e->has_seasons || sidx + 1 >= e->nseasons) return -1;
        ns = sidx + 1;
        if (e->seasons[ns].n <= 0) return -2;         /* its episodes are not loaded yet */
        ni = 0;
    }
    const vseason_t *tg = &e->seasons[ns];
    const int tnum = tg->num[ni];
    if (item_id && id_sz)
        snprintf(item_id, id_sz, "%d|%ce:%s:%d:%d", g_play_ep_portal, t, idbuf,
                 e->has_seasons ? ns : -1, tnum);
    if (title && title_sz) {
        const char *nm = e->name[0] ? e->name : "Series";
        if (e->has_seasons) snprintf(title, title_sz, "%s - S%dE%d", nm, ns + 1, tnum);
        else                snprintf(title, title_sz, "%s - E%d", nm, tnum);
    }
    return 0;
}

void stalker_note_title(const char *title)
{
    if (!title || !g_play_ep[0]) return;
    const char *dash = strstr(title, " - ");
    const char *last = NULL;
    for (const char *d = dash; d; d = strstr(d + 3, " - ")) last = d;
    if (!last || last == title) return;
    char idbuf[EVO_PROVIDER_MAX_ITEM_ID];
    episode_series_id(g_play_ep, idbuf, sizeof idbuf);
    vc_t *e = vc_find(g_play_ep[0], idbuf);
    if (!e || e->name[0]) return;
    size_t n = (size_t)(last - title);
    if (n >= sizeof e->name) n = sizeof e->name - 1;
    memcpy(e->name, title, n);
    e->name[n] = '\0';
    evo_title_normalize_spaces(e->name);
}

int stalker_is_playing(const char *url)
{
    return url && g_play_url[0] && strcmp(url, g_play_url) == 0;
}

int stalker_zap_neighbor(int dir, char *item_id, size_t id_sz, char *title, size_t title_sz)
{
    if (g_play_portal < 0 || g_play_portal >= g_np || g_active != g_play_portal || g_ch_n <= 0)
        return -1;
    int *idx = (int *)malloc((size_t)g_ch_n * sizeof *idx);
    if (!idx) return -1;
    int m = channel_order(g_play_want, idx);
    if (m < 2) { free(idx); return -1; }
    int at = -1;
    for (int j = 0; j < m && at < 0; ++j)
        if (strcmp(g_ch[idx[j]].id, g_play_id) == 0) at = j;
    if (at < 0) at = dir > 0 ? -1 : 0;
    int j = ((at + (dir < 0 ? -1 : 1)) % m + m) % m;
    const stalker_ch_t *c = &g_ch[idx[j]];
    if (item_id && id_sz) snprintf(item_id, id_sz, "%d|ch:%s", g_play_portal, c->id);
    if (title && title_sz) snprintf(title, title_sz, "%s", c->name[0] ? c->name : "Channel");
    free(idx);
    return 0;
}

int stalker_restore_categories(char *msg, size_t msg_sz)
{
    memset(g_prefs, 0, sizeof g_prefs);
    chord_free_all();                                 /* the channel orders too */
    chord_save();
    g_vis_n[0] = g_vis_n[1] = g_vis_n[2] = 0;
    stalker_save_config();
    say(msg, msg_sz, "All categories are back, in the portal's own order%s", "");
    return 0;
}

static const char *stalker_ui_bundle_url(void)
{
    return NULL;                                      /* embedded fallback skin */
}

const evo_provider_t evo_provider_stalker = {
    .id              = "stalker",
    .name            = "Stalker / Ministra",
    .icon            = "icon_tv.png",
    .caps            = EVO_PROVIDER_CAP_CATALOG | EVO_PROVIDER_CAP_SEARCH |
                       EVO_PROVIDER_CAP_RESOLVE |
                       EVO_PROVIDER_CAP_AUTH    | EVO_PROVIDER_CAP_UI |
                       EVO_PROVIDER_CAP_CONFIG  | EVO_PROVIDER_CAP_LIVE,
    .api_version     = EVO_PROVIDER_API_VERSION,
    .init            = stalker_init,
    .shutdown        = stalker_shutdown,
    .is_configured   = stalker_is_configured,
    .auth            = stalker_auth,
    .list_catalog    = stalker_list_catalog,
    .search          = stalker_search,
    .resolve         = stalker_resolve,
    .report_progress = NULL,
    .ui_bundle_url   = stalker_ui_bundle_url,
    .get_source      = stalker_get_source,
    .set_source      = stalker_set_source,
    .web_ui_url      = NULL,
    .web_ui_path     = NULL,
    .web_ui_hook     = NULL,
    .is_signed_in    = stalker_is_configured,
    .sign_out        = stalker_sign_out,
};
