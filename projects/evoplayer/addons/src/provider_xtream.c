/*
 * provider_xtream.c — Xtream Codes IPTV provider for EVO Player (#93).
 *
 * Implements the Xtream Codes player API (player_api.php) behind the
 * evo_provider_t vtable. Supports Live TV, VOD (Movies), and Series with
 * full season/episode drilling, posters/covers, and multi-stream format
 * selection (HLS .m3u8 vs MPEG-TS .ts).
 *
 * Credentials are read from and persisted to xtream.conf (with USB fallback
 * /mnt/usb0/.evo_xtream.conf), and are NEVER printed to evo.log or PROV_LOG.
 */
#include "evo_epg.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <stdbool.h>
#include <time.h>

#include "addon_xtream.h"
#include "evo_provider.h"
#include "evo_net.h"
#include "cJSON.h"
#include "evo_data_path.h"
#include "evo_provider_log.h"

#define XTREAM_CONF "xtream.conf"
#include "evo_recent.h"
#include "evo_title_tag.h"
#include "evo_parental.h"

/* Case-insensitive "does hay contain needle". The console's libc leaves
 * strcasestr as a NULL import: calling it crashes (see evo_webui.c). */
static int xt_ci_contains(const char *hay, const char *needle)
{
    if (!hay || !needle) return 0;
    const size_t n = strlen(needle);
    if (n == 0) return 1;
    for (; *hay; ++hay)
        if (strncasecmp(hay, needle, n) == 0) return 1;
    return 0;
}
static void parental_mark(const char *provider, evo_provider_item_t *it);
#define XTREAM_CONF_USB "/mnt/usb0/.evo_xtream.conf"

static xtream_config_t g_cfg;
static char g_source[256];
static void xt_accts_load(void);
static void xt_accts_sync(void);

/* ------------------------------------------------------------------------- */
/* Credential masking for safe logging                                       */
/* ------------------------------------------------------------------------- */

static void mask_url(const char *url, char *out, size_t out_sz)
{
    if (!url || !out || out_sz == 0) return;
    snprintf(out, out_sz, "%s", url);

    /* Mask username=... */
    char *u = strstr(out, "username=");
    if (u) {
        u += 9;
        char *amp = strchr(u, '&');
        char tail[256] = {0};
        if (amp) snprintf(tail, sizeof tail, "%s", amp);
        snprintf(u, out_sz - (size_t)(u - out), "***%s", tail);
    }

    /* Mask password=... */
    char *p = strstr(out, "password=");
    if (p) {
        p += 9;
        char *amp = strchr(p, '&');
        char tail[256] = {0};
        if (amp) snprintf(tail, sizeof tail, "%s", amp);
        snprintf(p, out_sz - (size_t)(p - out), "***%s", tail);
    }

    /* Mask /live/user/pass/ or /movie/user/pass/ or /series/user/pass/ */
    static const char *prefixes[] = { "/live/", "/movie/", "/series/", "/timeshift/", NULL };
    for (int i = 0; prefixes[i]; ++i) {
        char *loc = strstr(out, prefixes[i]);
        if (loc) {
            char *u_start = loc + strlen(prefixes[i]);
            char *slash1 = strchr(u_start, '/');
            if (slash1) {
                char *slash2 = strchr(slash1 + 1, '/');
                if (slash2) {
                    char tail[256] = {0};
                    snprintf(tail, sizeof tail, "%s", slash2);
                    snprintf(u_start, out_sz - (size_t)(u_start - out), "***/***%s", tail);
                }
            }
        }
    }
}

/* ------------------------------------------------------------------------- */
/* Config persistence                                                        */
/* ------------------------------------------------------------------------- */

static void trim(char *s)
{
    if (!s) return;
    size_t n = strlen(s);
    while (n && (s[n - 1] == '\r' || s[n - 1] == '\n' || s[n - 1] == ' ' || s[n - 1] == '\t'))
        s[--n] = '\0';
    size_t lead = 0;
    while (s[lead] == ' ' || s[lead] == '\t') lead++;
    if (lead) memmove(s, s + lead, n - lead + 1);
}

int xtream_init(void)
{
    memset(&g_cfg, 0, sizeof g_cfg);
    g_cfg.port = 80;
    snprintf(g_cfg.stream_format, sizeof g_cfg.stream_format, "m3u8");

    const char *path = evo_data_path(XTREAM_CONF);
    FILE *f = fopen(path, "r");
    if (!f) f = fopen(XTREAM_CONF_USB, "r");
    if (!f) { xt_accts_load(); return 0; }

    char line[512];
    while (fgets(line, sizeof line, f)) {
        trim(line);
        if (line[0] == '#' || line[0] == '\0') continue;
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        char *k = line, *v = eq + 1;
        trim(k); trim(v);

        if (strcmp(k, "host") == 0)
            snprintf(g_cfg.host, sizeof g_cfg.host, "%s", v);
        else if (strcmp(k, "port") == 0)
            g_cfg.port = atoi(v);
        else if (strcmp(k, "username") == 0)
            snprintf(g_cfg.username, sizeof g_cfg.username, "%s", v);
        else if (strcmp(k, "password") == 0)
            snprintf(g_cfg.password, sizeof g_cfg.password, "%s", v);
        else if (strcmp(k, "use_https") == 0)
            g_cfg.use_https = (atoi(v) != 0);
        else if (strcmp(k, "stream_format") == 0)
            snprintf(g_cfg.stream_format, sizeof g_cfg.stream_format, "%s", v);
        else if (strcmp(k, "status") == 0)
            snprintf(g_cfg.status, sizeof g_cfg.status, "%s", v);
        else if (strcmp(k, "exp_date") == 0)
            snprintf(g_cfg.exp_date, sizeof g_cfg.exp_date, "%s", v);
    }
    fclose(f);

    PROV_LOG("xtream: loaded config host='%s' port=%d https=%d format='%s'",
             g_cfg.host, g_cfg.port, g_cfg.use_https ? 1 : 0, g_cfg.stream_format);
    xt_accts_load();
    return 0;
}

int xtream_save_config(void)
{
    const char *path = evo_data_path(XTREAM_CONF);
    FILE *f = fopen(path, "w");
    if (!f) return -1;

    fprintf(f, "# EVO Player - Xtream Codes Configuration\n");
    fprintf(f, "host=%s\n", g_cfg.host);
    fprintf(f, "port=%d\n", g_cfg.port > 0 ? g_cfg.port : (g_cfg.use_https ? 443 : 80));
    fprintf(f, "username=%s\n", g_cfg.username);
    fprintf(f, "password=%s\n", g_cfg.password);
    fprintf(f, "use_https=%d\n", g_cfg.use_https ? 1 : 0);
    fprintf(f, "stream_format=%s\n", g_cfg.stream_format[0] ? g_cfg.stream_format : "m3u8");
    if (g_cfg.status[0])   fprintf(f, "status=%s\n", g_cfg.status);
    if (g_cfg.exp_date[0]) fprintf(f, "exp_date=%s\n", g_cfg.exp_date);
    fclose(f);
    xt_accts_sync();
    return 0;
}

xtream_config_t *xtream_get_config(void)
{
    return &g_cfg;
}

static void xc_clear_all(void);

void xtream_set_server(const char *host, int port, const char *username, const char *password, bool use_https)
{
    xc_clear_all();                   /* another account: its own lists */
    if (host)     snprintf(g_cfg.host, sizeof g_cfg.host, "%s", host);
    if (port > 0) g_cfg.port = port;
    if (username) snprintf(g_cfg.username, sizeof g_cfg.username, "%s", username);
    if (password) snprintf(g_cfg.password, sizeof g_cfg.password, "%s", password);
    g_cfg.use_https = use_https;
    if (!g_cfg.stream_format[0])
        snprintf(g_cfg.stream_format, sizeof g_cfg.stream_format, "m3u8");
}

void xtream_set_stream_format(const char *fmt)
{
    if (fmt && (strcmp(fmt, "ts") == 0 || strcmp(fmt, "m3u8") == 0)) {
        snprintf(g_cfg.stream_format, sizeof g_cfg.stream_format, "%s", fmt);
        xtream_save_config();
    }
}

/* ------------------------------------------------------------------------- */
/* URL Construction (Internal)                                               */
/* ------------------------------------------------------------------------- */

static void build_api_url(char *buf, size_t sz, const char *action_query)
{
    const char *scheme = g_cfg.use_https ? "https" : "http";
    int default_port = g_cfg.use_https ? 443 : 80;
    if (g_cfg.port > 0 && g_cfg.port != default_port) {
        if (action_query && *action_query) {
            snprintf(buf, sz, "%s://%s:%d/player_api.php?username=%s&password=%s&%s",
                     scheme, g_cfg.host, g_cfg.port, g_cfg.username, g_cfg.password, action_query);
        } else {
            snprintf(buf, sz, "%s://%s:%d/player_api.php?username=%s&password=%s",
                     scheme, g_cfg.host, g_cfg.port, g_cfg.username, g_cfg.password);
        }
    } else {
        if (action_query && *action_query) {
            snprintf(buf, sz, "%s://%s/player_api.php?username=%s&password=%s&%s",
                     scheme, g_cfg.host, g_cfg.username, g_cfg.password, action_query);
        } else {
            snprintf(buf, sz, "%s://%s/player_api.php?username=%s&password=%s",
                     scheme, g_cfg.host, g_cfg.username, g_cfg.password);
        }
    }
}

/* The account's full XMLTV guide: <server>/xmltv.php?username=..&password=.. */
static void build_xmltv_url(char *buf, size_t sz)
{
    const char *scheme = g_cfg.use_https ? "https" : "http";
    int default_port = g_cfg.use_https ? 443 : 80;
    if (g_cfg.port > 0 && g_cfg.port != default_port)
        snprintf(buf, sz, "%s://%s:%d/xmltv.php?username=%s&password=%s",
                 scheme, g_cfg.host, g_cfg.port, g_cfg.username, g_cfg.password);
    else
        snprintf(buf, sz, "%s://%s/xmltv.php?username=%s&password=%s",
                 scheme, g_cfg.host, g_cfg.username, g_cfg.password);
}

/* ------------------------------------------------------------------------- */
/* Authentication                                                            */
/* ------------------------------------------------------------------------- */

typedef struct {
    xtream_auth_cb cb;
    void *ud;
} auth_ctx_t;

static void on_auth_resp(int success, int status, const char *body, size_t len, void *ud)
{
    (void)len;
    auth_ctx_t *ctx = (auth_ctx_t *)ud;
    if (!ctx) return;

    if (!success || status < 200 || status >= 300 || !body) {
        g_cfg.is_connected = false;
        if (ctx->cb) ctx->cb(0, "Connection to Xtream server failed", ctx->ud);
        free(ctx);
        return;
    }

    cJSON *root = cJSON_Parse(body);
    if (!root) {
        g_cfg.is_connected = false;
        if (ctx->cb) ctx->cb(0, "Invalid JSON from Xtream server", ctx->ud);
        free(ctx);
        return;
    }

    cJSON *uinfo = cJSON_GetObjectItem(root, "user_info");
    int auth = 0;
    const char *stat = "";
    const char *exp = "";

    if (uinfo) {
        cJSON *a = cJSON_GetObjectItem(uinfo, "auth");
        if (a) {
            if (cJSON_IsNumber(a)) auth = (a->valueint != 0);
            else if (cJSON_IsString(a) && a->valuestring) auth = (strcmp(a->valuestring, "1") == 0);
        }
        cJSON *st = cJSON_GetObjectItem(uinfo, "status");
        if (st && cJSON_IsString(st) && st->valuestring) stat = st->valuestring;
        cJSON *ex = cJSON_GetObjectItem(uinfo, "exp_date");
        if (ex && cJSON_IsString(ex) && ex->valuestring) exp = ex->valuestring;
    }

    if (auth != 0 && (stat[0] == '\0' || strcasecmp(stat, "Active") == 0)) {
        g_cfg.is_connected = true;
        if (stat[0]) snprintf(g_cfg.status, sizeof g_cfg.status, "%s", stat);
        if (exp[0])  snprintf(g_cfg.exp_date, sizeof g_cfg.exp_date, "%s", exp);
        xtream_save_config();
        if (ctx->cb) ctx->cb(1, "Account active", ctx->ud);
    } else if (auth == 0) {
        g_cfg.is_connected = false;
        if (ctx->cb) ctx->cb(0, "Invalid username or password", ctx->ud);
    } else {
        g_cfg.is_connected = false;
        char msg[96];
        snprintf(msg, sizeof msg, "Account status: %s", stat[0] ? stat : "Inactive");
        if (ctx->cb) ctx->cb(0, msg, ctx->ud);
    }

    cJSON_Delete(root);
    free(ctx);
}

int xtream_connect_async(xtream_auth_cb callback, void *userdata)
{
    if (!g_cfg.host[0] || !g_cfg.username[0] || !g_cfg.password[0]) {
        if (callback) callback(0, "Missing Xtream server credentials", userdata);
        return -1;
    }

    auth_ctx_t *ctx = (auth_ctx_t *)calloc(1, sizeof *ctx);
    if (!ctx) return -2;
    ctx->cb = callback;
    ctx->ud = userdata;

    char url[1024];
    build_api_url(url, sizeof url, NULL);

    char masked[1024];
    mask_url(url, masked, sizeof masked);
    PROV_LOG("xtream: connecting to %s", masked);

    return evo_net_request_async("GET", url, NULL, NULL, 0, on_auth_resp, ctx);
}

/* ------------------------------------------------------------------------- */
/* Catalog & Items                                                           */
/* ------------------------------------------------------------------------- */


/* ------------------------------------------------------------------------- */
/* Lists kept a day, like the Stalker portal's                               */
/* ------------------------------------------------------------------------- */
/*
 * Categories, the films / series of a category, a series' episodes and the
 * whole lists are kept under <data>/xtream_cache, one file per request (and
 * account), and answered from there for 24 hours. When the panel does not
 * answer, an older copy is used rather than an empty screen. Stream links and
 * the sign-in are never kept.
 */
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>

#define XC_TTL_SEC   (24 * 60 * 60)
#define XC_PRUNE_SEC (7 * 24 * 60 * 60)
#define XC_MAX_BYTES (48 * 1024 * 1024)

static int xc_dir(char *out, size_t sz)
{
    snprintf(out, sz, "%s", evo_data_path("xtream_cache"));
    struct stat st;
    if (stat(out, &st) == 0) return 0;
    return mkdir(out, 0777) == 0 ? 0 : -1;
}

static int xc_path(const char *url, char *out, size_t sz)
{
    char dir[512];
    if (xc_dir(dir, sizeof dir) != 0) return -1;
    unsigned long long h = 1469598103934665603ull;
    for (const char *c = url; *c; ++c) h = (h ^ (unsigned char)*c) * 1099511628211ull;
    const int w = snprintf(out, sz, "%s/%016llx.json", dir, h);
    return (w > 0 && (size_t)w < sz) ? 0 : -1;
}

static void xc_clear_all(void)
{
    char dir[512];
    if (xc_dir(dir, sizeof dir) != 0) return;
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

/* Settings > Clear cache. */
void xtream_clear_cache(void)
{
    xc_clear_all();
    PROV_LOG("xtream: cache cleared");
}

static void xc_prune(void)
{
    static int done;
    if (done) return;
    done = 1;
    char dir[512];
    if (xc_dir(dir, sizeof dir) != 0) return;
    DIR *d = opendir(dir);
    if (!d) return;
    const time_t now = time(NULL);
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (de->d_name[0] == '.') continue;
        char path[800];
        snprintf(path, sizeof path, "%s/%s", dir, de->d_name);
        struct stat st;
        if (stat(path, &st) == 0 && now - st.st_mtime > XC_PRUNE_SEC) unlink(path);
    }
    closedir(d);
}

static char *xc_read(const char *path, size_t *len_out, long *age_out)
{
    struct stat st;
    if (stat(path, &st) != 0 || st.st_size <= 0 || st.st_size > XC_MAX_BYTES) return NULL;
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

/* A real list or object, not an empty one or a refusal. */
static int xc_worth_keeping(const char *body, size_t len)
{
    if (!body || len < 3 || len > XC_MAX_BYTES) return 0;
    const char *c = body;
    while (*c == ' ' || *c == '\n' || *c == '\r' || *c == '\t') ++c;
    if (*c != '[' && *c != '{') return 0;
    if (strncmp(c, "[]", 2) == 0 || strncmp(c, "{}", 2) == 0) return 0;
    return !strstr(body, "\"auth\":0");
}

typedef struct {
    evo_net_cb cb;
    void      *ctx;
    char       path[800];
} xc_wait_t;

static void on_xc_net(int success, int status, const char *body, size_t len, void *ud)
{
    xc_wait_t *w = (xc_wait_t *)ud;
    const int good = success && status >= 200 && status < 300;
    if (good && xc_worth_keeping(body, len)) {
        char tmp[820];
        snprintf(tmp, sizeof tmp, "%s.tmp", w->path);
        FILE *f = fopen(tmp, "wb");
        if (f) {
            const int ok = fwrite(body, 1, len, f) == len;
            fclose(f);
            if (!ok || rename(tmp, w->path) != 0) unlink(tmp);
        }
        w->cb(success, status, body, len, w->ctx);
    } else {
        size_t n = 0;
        long age = 0;
        char *old = xc_read(w->path, &n, &age);
        if (old) {
            PROV_LOG("xtream: cache: panel did not answer (http=%d), using a copy %ld h old", status, age / 3600);
            w->cb(1, 200, old, n, w->ctx);
            free(old);
        } else {
            w->cb(success, status, body, len, w->ctx);
        }
    }
    free(w);
}

/* GET a list: from disk while it is under a day old, else from the panel. */
static int xt_get_cached(const char *url, evo_net_cb cb, void *ctx)
{
    xc_prune();
    xc_wait_t *w = (xc_wait_t *)calloc(1, sizeof *w);
    if (!w || xc_path(url, w->path, sizeof w->path) != 0) {
        free(w);
        return evo_net_request_async("GET", url, NULL, NULL, 0, cb, ctx);
    }
    size_t n = 0;
    long age = 0;
    char *body = xc_read(w->path, &n, &age);
    if (body && age >= 0 && age < XC_TTL_SEC && xc_worth_keeping(body, n)) {
        free(w);
        cb(1, 200, body, n, ctx);              /* the same path a panel answer takes */
        free(body);
        return 0;
    }
    free(body);
    w->cb = cb;
    w->ctx = ctx;
    const int rc = evo_net_request_async("GET", url, NULL, NULL, 0, on_xc_net, w);
    if (rc != 0) free(w);
    return rc;
}

typedef struct {
    evo_provider_items_cb cb;
    void *ud;
    char parent_id[EVO_PROVIDER_MAX_ITEM_ID];
} catalog_ctx_t;

static void on_categories_resp(int success, int status, const char *body, size_t len, void *ud)
{
    (void)len;
    catalog_ctx_t *ctx = (catalog_ctx_t *)ud;
    if (!ctx) return;

    if (!success || status < 200 || status >= 300 || !body) {
        if (ctx->cb) ctx->cb(0, NULL, 0, 0, ctx->ud);
        free(ctx);
        return;
    }

    cJSON *arr = cJSON_Parse(body);
    if (!arr || !cJSON_IsArray(arr)) {
        if (ctx->cb) ctx->cb(0, NULL, 0, 0, ctx->ud);
        if (arr) cJSON_Delete(arr);
        free(ctx);
        return;
    }

    int count = cJSON_GetArraySize(arr);
    if (count <= 0) {
        if (ctx->cb) ctx->cb(1, NULL, 0, 0, ctx->ud);
        cJSON_Delete(arr);
        free(ctx);
        return;
    }

    if (count > EVO_PROVIDER_PAGE_MAX) count = EVO_PROVIDER_PAGE_MAX;
    evo_provider_item_t *items = (evo_provider_item_t *)calloc((size_t)count, sizeof *items);
    if (!items) {
        if (ctx->cb) ctx->cb(0, NULL, 0, 0, ctx->ud);
        cJSON_Delete(arr);
        free(ctx);
        return;
    }

    const char *prefix = (strcmp(ctx->parent_id, "live") == 0) ? "live_cat:" :
                         (strcmp(ctx->parent_id, "vod") == 0)  ? "vod_cat:" : "series_cat:";

    for (int i = 0; i < count; ++i) {
        cJSON *obj = cJSON_GetArrayItem(arr, i);
        if (!obj) continue;
        evo_provider_item_clear(&items[i]);

        cJSON *cid = cJSON_GetObjectItem(obj, "category_id");
        cJSON *cname = cJSON_GetObjectItem(obj, "category_name");

        const char *id_str = cid ? (cJSON_IsString(cid) ? cid->valuestring : "") : "";
        char id_buf[32] = {0};
        if (!id_str[0] && cid && cJSON_IsNumber(cid)) {
            snprintf(id_buf, sizeof id_buf, "%d", cid->valueint);
            id_str = id_buf;
        }

        snprintf(items[i].id, sizeof items[i].id, "%s%s", prefix, id_str);
        snprintf(items[i].parent_id, sizeof items[i].parent_id, "%s", ctx->parent_id);
        snprintf(items[i].title, sizeof items[i].title, "%s",
                 (cname && cname->valuestring) ? cname->valuestring : "Category");
        parental_mark("xtream", &items[i]);
        items[i].kind = EVO_MEDIA_FOLDER;
        items[i].is_folder = 1;
    }

    if (ctx->cb) ctx->cb(1, items, count, 0, ctx->ud);
    free(items);
    cJSON_Delete(arr);
    free(ctx);
}

/* ------------------------------------------------------------------------- */
/* Live channels: guide id and archive, for the TV guide grid and catch-up    */
/* ------------------------------------------------------------------------- */
static double xt_num(const cJSON *o, const char *key);
#define XT_CH_SLOTS 16384                       /* a power of two */
typedef struct {
    char sid[16];
    char epg[64];
    unsigned char archive_days;                 /* tv_archive_duration, 0 = none */
} xt_ch_t;
static xt_ch_t *g_xt_ch;
static int      g_xt_ch_n;

static unsigned xt_hash(const char *s)
{
    unsigned h = 2166136261u;
    while (*s) { h ^= (unsigned char)*s++; h *= 16777619u; }
    return h;
}

static xt_ch_t *xt_ch_slot(const char *sid, int create)
{
    if (!sid || !sid[0] || strlen(sid) >= sizeof g_xt_ch[0].sid) return NULL;
    if (!g_xt_ch) {
        if (!create) return NULL;
        g_xt_ch = (xt_ch_t *)calloc(XT_CH_SLOTS, sizeof *g_xt_ch);
        if (!g_xt_ch) return NULL;
    }
    unsigned h = xt_hash(sid) & (XT_CH_SLOTS - 1);
    for (int probe = 0; probe < XT_CH_SLOTS; ++probe, h = (h + 1) & (XT_CH_SLOTS - 1)) {
        if (!g_xt_ch[h].sid[0]) {
            if (!create || g_xt_ch_n >= XT_CH_SLOTS * 3 / 4) return NULL;
            snprintf(g_xt_ch[h].sid, sizeof g_xt_ch[h].sid, "%s", sid);
            g_xt_ch_n++;
            return &g_xt_ch[h];
        }
        if (strcmp(g_xt_ch[h].sid, sid) == 0) return &g_xt_ch[h];
    }
    return NULL;
}

/*
 * The panel's clock. Catch-up asks for a programme by its start in the
 * server's own time ("2026-10-12:20-00"), which the guide gives as text next
 * to the Unix time: the difference is the server's offset from UTC.
 */
static long g_xt_tz_off;
static int  g_xt_tz_known;

/* "2026-10-12 20:00:00" read as if it were UTC. -1 when it is not that. */
static long long xt_civil(const char *s)
{
    int y, mo, d, h, mi, se = 0;
    if (!s || sscanf(s, "%d-%d-%d %d:%d:%d", &y, &mo, &d, &h, &mi, &se) < 5) return -1;
    int yy = y - (mo <= 2);
    int era = (yy >= 0 ? yy : yy - 399) / 400;
    unsigned yoe = (unsigned)(yy - era * 400);
    unsigned doy = (unsigned)((153 * (mo + (mo > 2 ? -3 : 9)) + 2) / 5 + d - 1);
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    long long days = (long long)era * 146097 + (long long)doe - 719468;
    return days * 86400 + h * 3600 + mi * 60 + se;
}

static void xt_note_clock(const char *civil, long long unix_ts)
{
    const long long c = xt_civil(civil);
    if (c < 0 || unix_ts <= 0) return;
    long off = (long)(c - unix_ts);
    off = ((off + (off >= 0 ? 450 : -450)) / 900) * 900;   /* whole quarter hours */
    if (off < -14 * 3600 || off > 14 * 3600) return;
    g_xt_tz_off = off;
    g_xt_tz_known = 1;
}

/* For the TV guide grid: a live channel's guide id and days of archive.
 * 0 when item_id is a live channel of this account. */
int xtream_channel_info(const char *item_id, char *epg_id, size_t esz, int *archive_days)
{
    const char *bar = item_id ? strchr(item_id, '|') : NULL;
    if (bar) item_id = bar + 1;
    if (!item_id || strncmp(item_id, "live_stream:", 12) != 0) return -1;
    if (epg_id && esz) epg_id[0] = 0;
    if (archive_days) *archive_days = 0;
    xt_ch_t *c = xt_ch_slot(item_id + 12, 0);
    if (c) {
        if (epg_id && esz) snprintf(epg_id, esz, "%s", c->epg);
        if (archive_days) *archive_days = c->archive_days;
    }
    return 0;
}

static void on_live_streams_resp(int success, int status, const char *body, size_t len, void *ud)
{
    (void)len;
    catalog_ctx_t *ctx = (catalog_ctx_t *)ud;
    if (!ctx) return;

    if (!success || status < 200 || status >= 300 || !body) {
        if (ctx->cb) ctx->cb(0, NULL, 0, 0, ctx->ud);
        free(ctx);
        return;
    }

    cJSON *arr = cJSON_Parse(body);
    if (!arr || !cJSON_IsArray(arr)) {
        if (ctx->cb) ctx->cb(0, NULL, 0, 0, ctx->ud);
        if (arr) cJSON_Delete(arr);
        free(ctx);
        return;
    }

    int count = cJSON_GetArraySize(arr);
    if (count <= 0) {
        if (ctx->cb) ctx->cb(1, NULL, 0, 0, ctx->ud);
        cJSON_Delete(arr);
        free(ctx);
        return;
    }

    if (count > EVO_PROVIDER_PAGE_MAX) count = EVO_PROVIDER_PAGE_MAX;
    evo_provider_item_t *items = (evo_provider_item_t *)calloc((size_t)count, sizeof *items);
    if (!items) {
        if (ctx->cb) ctx->cb(0, NULL, 0, 0, ctx->ud);
        cJSON_Delete(arr);
        free(ctx);
        return;
    }

    for (int i = 0; i < count; ++i) {
        cJSON *obj = cJSON_GetArrayItem(arr, i);
        if (!obj) continue;
        evo_provider_item_clear(&items[i]);

        cJSON *sid = cJSON_GetObjectItem(obj, "stream_id");
        cJSON *name = cJSON_GetObjectItem(obj, "name");
        cJSON *icon = cJSON_GetObjectItem(obj, "stream_icon");

        char id_buf[32] = {0};
        const char *id_str = sid ? (cJSON_IsString(sid) ? sid->valuestring : "") : "";
        if (!id_str[0] && sid && cJSON_IsNumber(sid)) {
            snprintf(id_buf, sizeof id_buf, "%d", sid->valueint);
            id_str = id_buf;
        }

        snprintf(items[i].id, sizeof items[i].id, "live_stream:%s", id_str);
        snprintf(items[i].parent_id, sizeof items[i].parent_id, "%s", ctx->parent_id);
        snprintf(items[i].title, sizeof items[i].title, "%s",
                 (name && name->valuestring) ? name->valuestring : "Channel");
        if (icon && icon->valuestring && icon->valuestring[0]) {
            snprintf(items[i].art_url, sizeof items[i].art_url, "%s", icon->valuestring);
        }
        cJSON *now_obj = cJSON_GetObjectItem(obj, "now_title");
        if (!now_obj) now_obj = cJSON_GetObjectItem(obj, "now");
        if (!now_obj) now_obj = cJSON_GetObjectItem(obj, "current_show");
        if (!now_obj) now_obj = cJSON_GetObjectItem(obj, "prog");

        cJSON *next_obj = cJSON_GetObjectItem(obj, "next_title");
        if (!next_obj) next_obj = cJSON_GetObjectItem(obj, "next");
        if (!next_obj) next_obj = cJSON_GetObjectItem(obj, "next_show");

        cJSON *epg_id = cJSON_GetObjectItem(obj, "epg_channel_id");
        {
            xt_ch_t *ch = xt_ch_slot(id_str, 1);
            if (ch) {
                snprintf(ch->epg, sizeof ch->epg, "%s",
                         (epg_id && cJSON_IsString(epg_id) && epg_id->valuestring) ? epg_id->valuestring : "");
                const int arch = (int)xt_num(obj, "tv_archive");
                int days = (int)xt_num(obj, "tv_archive_duration");
                if (arch && days <= 0) days = 1;
                ch->archive_days = (unsigned char)(arch ? (days > 30 ? 30 : days) : 0);
            }
        }

        if (now_obj && now_obj->valuestring && now_obj->valuestring[0]) {
            snprintf(items[i].now_title, sizeof items[i].now_title, "%s", now_obj->valuestring);
        }
        if (next_obj && next_obj->valuestring && next_obj->valuestring[0]) {
            snprintf(items[i].next_title, sizeof items[i].next_title, "%s", next_obj->valuestring);
        }

        if (items[i].now_title[0]) {
            if (items[i].next_title[0])
                snprintf(items[i].subtitle, sizeof items[i].subtitle, "%s | Next: %s",
                         items[i].now_title, items[i].next_title);
            else
                snprintf(items[i].subtitle, sizeof items[i].subtitle, "%s", items[i].now_title);
        } else if (epg_id && epg_id->valuestring && epg_id->valuestring[0]) {
            snprintf(items[i].subtitle, sizeof items[i].subtitle, "EPG: %s", epg_id->valuestring);
        } else {
            snprintf(items[i].subtitle, sizeof items[i].subtitle, "Live Broadcast");
        }

        items[i].kind = EVO_MEDIA_STREAM;
        items[i].is_live = 1;
        items[i].is_folder = 0;

        /* Now/next from the account's own guide (xmltv.php), then the TV
         * guide from Settings. */
        evo_epg_fill_item(&items[i], "xtream",
                          (epg_id && epg_id->valuestring) ? epg_id->valuestring : NULL,
                          items[i].title);
    }

    if (ctx->cb) ctx->cb(1, items, count, 0, ctx->ud);
    free(items);
    cJSON_Delete(arr);
    free(ctx);
}


/* ------------------------------------------------------------------------- */
/* Posters and watched state, as the Stalker portal has them                  */
/* ------------------------------------------------------------------------- */
/*
 * A row's poster is remembered by its id when the list is shown, so the
 * resolved stream can carry it and Continue Watching shows it on Home. And a
 * row Continue Watching already knows gets its check mark or progress bar.
 */
#define XT_ART_SLOTS 512
typedef struct {
    char id[EVO_PROVIDER_MAX_ITEM_ID];
    char url[EVO_PROVIDER_MAX_ART_URL];
    char year[8];
    int  eps;                                   /* episodes in its season */
    char plot[EVO_PROVIDER_MAX_OVERVIEW];
} xt_art_t;
static xt_art_t g_xt_art[XT_ART_SLOTS];
static int      g_xt_art_next;

static xt_art_t *xt_slot(const char *id, int create)
{
    if (!id || !id[0]) return NULL;
    for (int i = 0; i < XT_ART_SLOTS; ++i)
        if (strcmp(g_xt_art[i].id, id) == 0) return &g_xt_art[i];
    if (!create) return NULL;
    xt_art_t *e = &g_xt_art[g_xt_art_next];
    g_xt_art_next = (g_xt_art_next + 1) % XT_ART_SLOTS;
    memset(e, 0, sizeof *e);
    snprintf(e->id, sizeof e->id, "%s", id);
    return e;
}

static void xt_art_put(const char *id, const char *url)
{
    if (!url || strncmp(url, "http", 4) != 0) return;
    xt_art_t *e = xt_slot(id, 1);
    if (e) snprintf(e->url, sizeof e->url, "%s", url);
}

/* What Home's big tile says: the year, the season's episode count, the plot. */
static void xt_info_put(const char *id, const char *year, int eps, const char *plot)
{
    xt_art_t *e = xt_slot(id, 1);
    if (!e) return;
    if (year && year[0]) snprintf(e->year, sizeof e->year, "%.4s", year);
    if (eps > 0) e->eps = eps;
    if (plot && plot[0]) snprintf(e->plot, sizeof e->plot, "%s", plot);
}

static void xt_art_get(const char *id, char *out, size_t sz)
{
    out[0] = '\0';
    xt_art_t *e = xt_slot(id, 0);
    if (e) snprintf(out, sz, "%s", e->url);
}

static void xt_choice_info(const char *id, evo_stream_choice_t *c)
{
    xt_art_get(id, c->art_url, sizeof c->art_url);
    xt_art_t *e = xt_slot(id, 0);
    if (!e) return;
    snprintf(c->year, sizeof c->year, "%s", e->year);
    c->ep_total = e->eps;
    snprintf(c->overview, sizeof c->overview, "%s", e->plot);
}

static void xt_series_remember(const char *sid, const char *name, const cJSON *episodes);

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

/* Search and Home: is this row's category locked? */
static int xt_hidden(const cJSON *obj, const char *prefix, const char *name)
{
    char cat[32] = "", key[48];
    const cJSON *v = cJSON_GetObjectItem(obj, "category_id");
    if (v && cJSON_IsString(v) && v->valuestring) snprintf(cat, sizeof cat, "%s", v->valuestring);
    else if (v && cJSON_IsNumber(v))             snprintf(cat, sizeof cat, "%d", v->valueint);
    snprintf(key, sizeof key, "%s%s", prefix, cat);
    return evo_parental_hides("xtream", cat[0] ? key : "", name);
}

static void xt_fill_watch_state(evo_provider_item_t *it)
{
    char key[sizeof recent_files[0].path], legacy[sizeof recent_files[0].path];
    snprintf(key, sizeof key, "evo://xtream/%s", it->id);
    /* Episodes watched before their id named the series: "series_ep:<ep>:<ext>". */
    snprintf(legacy, sizeof legacy, "%s", key);
    if (strncmp(it->id, "series_ep:", 10) == 0) {
        char *c = strrchr(legacy, ':');
        if (c && c > legacy + strlen("evo://xtream/series_ep:") && strchr(legacy + strlen("evo://xtream/series_ep:"), ':') != c)
            *c = '\0';
    }
    for (int i = 0; i < recent_file_count; ++i) {
        if (strcmp(recent_files[i].path, key) != 0 && strcmp(recent_files[i].path, legacy) != 0) continue;
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

static void on_vod_streams_resp(int success, int status, const char *body, size_t len, void *ud)
{
    (void)len;
    catalog_ctx_t *ctx = (catalog_ctx_t *)ud;
    if (!ctx) return;

    if (!success || status < 200 || status >= 300 || !body) {
        if (ctx->cb) ctx->cb(0, NULL, 0, 0, ctx->ud);
        free(ctx);
        return;
    }

    cJSON *arr = cJSON_Parse(body);
    if (!arr || !cJSON_IsArray(arr)) {
        if (ctx->cb) ctx->cb(0, NULL, 0, 0, ctx->ud);
        if (arr) cJSON_Delete(arr);
        free(ctx);
        return;
    }

    int count = cJSON_GetArraySize(arr);
    if (count <= 0) {
        if (ctx->cb) ctx->cb(1, NULL, 0, 0, ctx->ud);
        cJSON_Delete(arr);
        free(ctx);
        return;
    }

    if (count > EVO_PROVIDER_PAGE_MAX) count = EVO_PROVIDER_PAGE_MAX;
    evo_provider_item_t *items = (evo_provider_item_t *)calloc((size_t)count, sizeof *items);
    if (!items) {
        if (ctx->cb) ctx->cb(0, NULL, 0, 0, ctx->ud);
        cJSON_Delete(arr);
        free(ctx);
        return;
    }

    for (int i = 0; i < count; ++i) {
        cJSON *obj = cJSON_GetArrayItem(arr, i);
        if (!obj) continue;
        evo_provider_item_clear(&items[i]);
        if (strncmp(ctx->parent_id, "@new:", 5) == 0) {   /* Home: nothing from a locked category */
            cJSON *nm = cJSON_GetObjectItem(obj, "name");
            if (xt_hidden(obj, "vod_cat:", (nm && nm->valuestring) ? nm->valuestring : "")) continue;
        }

        cJSON *sid = cJSON_GetObjectItem(obj, "stream_id");
        cJSON *name = cJSON_GetObjectItem(obj, "name");
        cJSON *icon = cJSON_GetObjectItem(obj, "stream_icon");
        cJSON *ext = cJSON_GetObjectItem(obj, "container_extension");
        cJSON *rating = cJSON_GetObjectItem(obj, "rating");

        char id_buf[32] = {0};
        const char *id_str = sid ? (cJSON_IsString(sid) ? sid->valuestring : "") : "";
        if (!id_str[0] && sid && cJSON_IsNumber(sid)) {
            snprintf(id_buf, sizeof id_buf, "%d", sid->valueint);
            id_str = id_buf;
        }

        const char *ext_str = (ext && ext->valuestring && ext->valuestring[0]) ? ext->valuestring : "mp4";

        snprintf(items[i].id, sizeof items[i].id, "vod_stream:%s:%s", id_str, ext_str);
        snprintf(items[i].parent_id, sizeof items[i].parent_id, "%s", ctx->parent_id);
        snprintf(items[i].title, sizeof items[i].title, "%s",
                 (name && name->valuestring) ? name->valuestring : "Movie");
        if (rating && rating->valuestring && rating->valuestring[0]) {
            snprintf(items[i].subtitle, sizeof items[i].subtitle, "★ %s", rating->valuestring);
        }
        if (icon && icon->valuestring && icon->valuestring[0]) {
            snprintf(items[i].art_url, sizeof items[i].art_url, "%s", icon->valuestring);
        }
        evo_title_normalize_spaces(items[i].title);
        xt_art_put(items[i].id, items[i].art_url);
        {
            cJSON *yr = cJSON_GetObjectItem(obj, "year");
            if (yr && cJSON_IsString(yr) && yr->valuestring && yr->valuestring[0]) {
                xt_info_put(items[i].id, yr->valuestring, 0, NULL);
                snprintf(items[i].subtitle, sizeof items[i].subtitle, "%.4s", yr->valuestring);
            }
        }
        items[i].kind = EVO_MEDIA_VIDEO;
        items[i].is_live = 0;
        items[i].is_folder = 0;
        xt_fill_watch_state(&items[i]);
    }

    {   /* rows left out above have no id */
        int k = 0;
        for (int i = 0; i < count; ++i)
            if (items[i].id[0]) { if (k != i) items[k] = items[i]; k++; }
        count = k;
    }
    if (ctx->cb) ctx->cb(1, items, count, 0, ctx->ud);
    free(items);
    cJSON_Delete(arr);
    free(ctx);
}

static void on_series_resp(int success, int status, const char *body, size_t len, void *ud)
{
    (void)len;
    catalog_ctx_t *ctx = (catalog_ctx_t *)ud;
    if (!ctx) return;

    if (!success || status < 200 || status >= 300 || !body) {
        if (ctx->cb) ctx->cb(0, NULL, 0, 0, ctx->ud);
        free(ctx);
        return;
    }

    cJSON *arr = cJSON_Parse(body);
    if (!arr || !cJSON_IsArray(arr)) {
        if (ctx->cb) ctx->cb(0, NULL, 0, 0, ctx->ud);
        if (arr) cJSON_Delete(arr);
        free(ctx);
        return;
    }

    int count = cJSON_GetArraySize(arr);
    if (count <= 0) {
        if (ctx->cb) ctx->cb(1, NULL, 0, 0, ctx->ud);
        cJSON_Delete(arr);
        free(ctx);
        return;
    }

    if (count > EVO_PROVIDER_PAGE_MAX) count = EVO_PROVIDER_PAGE_MAX;
    evo_provider_item_t *items = (evo_provider_item_t *)calloc((size_t)count, sizeof *items);
    if (!items) {
        if (ctx->cb) ctx->cb(0, NULL, 0, 0, ctx->ud);
        cJSON_Delete(arr);
        free(ctx);
        return;
    }

    for (int i = 0; i < count; ++i) {
        cJSON *obj = cJSON_GetArrayItem(arr, i);
        if (!obj) continue;
        evo_provider_item_clear(&items[i]);
        if (strncmp(ctx->parent_id, "@new:", 5) == 0) {   /* Home: nothing from a locked category */
            cJSON *nm = cJSON_GetObjectItem(obj, "name");
            if (xt_hidden(obj, "series_cat:", (nm && nm->valuestring) ? nm->valuestring : "")) continue;
        }

        cJSON *sid = cJSON_GetObjectItem(obj, "series_id");
        cJSON *name = cJSON_GetObjectItem(obj, "name");
        cJSON *cover = cJSON_GetObjectItem(obj, "cover");
        cJSON *plot = cJSON_GetObjectItem(obj, "plot");
        cJSON *genre = cJSON_GetObjectItem(obj, "genre");

        char id_buf[32] = {0};
        const char *id_str = sid ? (cJSON_IsString(sid) ? sid->valuestring : "") : "";
        if (!id_str[0] && sid && cJSON_IsNumber(sid)) {
            snprintf(id_buf, sizeof id_buf, "%d", sid->valueint);
            id_str = id_buf;
        }

        snprintf(items[i].id, sizeof items[i].id, "series_show:%s", id_str);
        snprintf(items[i].parent_id, sizeof items[i].parent_id, "%s", ctx->parent_id);
        snprintf(items[i].title, sizeof items[i].title, "%s",
                 (name && name->valuestring) ? name->valuestring : "Series");
        if (genre && genre->valuestring && genre->valuestring[0]) {
            snprintf(items[i].subtitle, sizeof items[i].subtitle, "%s", genre->valuestring);
        }
        if (plot && plot->valuestring && plot->valuestring[0]) {
            snprintf(items[i].overview, sizeof items[i].overview, "%s", plot->valuestring);
        }
        if (cover && cover->valuestring && cover->valuestring[0]) {
            snprintf(items[i].art_url, sizeof items[i].art_url, "%s", cover->valuestring);
        }
        items[i].kind = EVO_MEDIA_FOLDER;
        items[i].is_folder = 1;
    }

    {   /* rows left out above have no id */
        int k = 0;
        for (int i = 0; i < count; ++i)
            if (items[i].id[0]) { if (k != i) items[k] = items[i]; k++; }
        count = k;
    }
    if (ctx->cb) ctx->cb(1, items, count, 0, ctx->ud);
    free(items);
    cJSON_Delete(arr);
    free(ctx);
}

static void on_series_info_resp(int success, int status, const char *body, size_t len, void *ud)
{
    (void)len;
    catalog_ctx_t *ctx = (catalog_ctx_t *)ud;
    if (!ctx) return;

    if (!success || status < 200 || status >= 300 || !body) {
        if (ctx->cb) ctx->cb(0, NULL, 0, 0, ctx->ud);
        free(ctx);
        return;
    }

    cJSON *root = cJSON_Parse(body);
    if (!root) {
        if (ctx->cb) ctx->cb(0, NULL, 0, 0, ctx->ud);
        free(ctx);
        return;
    }

    const char *series_id = "";
    if (strncmp(ctx->parent_id, "series_show:", 12) == 0)
        series_id = ctx->parent_id + 12;

    cJSON *seasons = cJSON_GetObjectItem(root, "seasons");
    cJSON *episodes = cJSON_GetObjectItem(root, "episodes");
    {
        /* The whole series in order, for L1 / R1 and Up next. */
        char sid[24] = "", sname[128] = "";
        const char *pid = ctx->parent_id;
        if (strncmp(pid, "series_show:", 12) == 0) snprintf(sid, sizeof sid, "%s", pid + 12);
        else if (strncmp(pid, "series_season:", 14) == 0) {
            snprintf(sid, sizeof sid, "%s", pid + 14);
            char *c = strchr(sid, ':');
            if (c) *c = '\0';
        }
        cJSON *si = cJSON_GetObjectItem(root, "info");
        cJSON *nm = si ? cJSON_GetObjectItem(si, "name") : NULL;
        if (nm && cJSON_IsString(nm) && nm->valuestring) {
            snprintf(sname, sizeof sname, "%s", nm->valuestring);
            evo_title_normalize_spaces(sname);
        }
        xt_series_remember(sid, sname, episodes);
    }

    /* If we are at the show level and there are seasons, emit Season folders */
    if (strncmp(ctx->parent_id, "series_show:", 12) == 0 && seasons && cJSON_IsArray(seasons) && cJSON_GetArraySize(seasons) > 1) {
        int count = cJSON_GetArraySize(seasons);
        if (count > EVO_PROVIDER_PAGE_MAX) count = EVO_PROVIDER_PAGE_MAX;
        evo_provider_item_t *items = (evo_provider_item_t *)calloc((size_t)count, sizeof *items);
        if (!items) {
            if (ctx->cb) ctx->cb(0, NULL, 0, 0, ctx->ud);
            cJSON_Delete(root);
            free(ctx);
            return;
        }

        for (int i = 0; i < count; ++i) {
            cJSON *s = cJSON_GetArrayItem(seasons, i);
            if (!s) continue;
            evo_provider_item_clear(&items[i]);

            cJSON *snum = cJSON_GetObjectItem(s, "season_number");
            cJSON *sname = cJSON_GetObjectItem(s, "name");
            cJSON *ep_count = cJSON_GetObjectItem(s, "episode_count");

            int num = snum ? (cJSON_IsNumber(snum) ? snum->valueint : atoi(snum->valuestring)) : (i + 1);
            snprintf(items[i].id, sizeof items[i].id, "series_season:%s:%d", series_id, num);
            snprintf(items[i].parent_id, sizeof items[i].parent_id, "%s", ctx->parent_id);
            snprintf(items[i].title, sizeof items[i].title, "%s",
                     (sname && sname->valuestring) ? sname->valuestring : "Season");
            if (ep_count) {
                int epc = cJSON_IsNumber(ep_count) ? ep_count->valueint : atoi(ep_count->valuestring);
                snprintf(items[i].subtitle, sizeof items[i].subtitle, "%d episode%s", epc, epc == 1 ? "" : "s");
            }
            items[i].kind = EVO_MEDIA_FOLDER;
            items[i].is_folder = 1;
        }

        if (ctx->cb) ctx->cb(1, items, count, 0, ctx->ud);
        free(items);
        cJSON_Delete(root);
        free(ctx);
        return;
    }

    /* Otherwise, emit episodes */
    char series_name[128] = "", series_cover[EVO_PROVIDER_MAX_ART_URL] = "";
    char series_plot[EVO_PROVIDER_MAX_OVERVIEW] = "", series_year[8] = "";
    {
        cJSON *sinfo = cJSON_GetObjectItem(root, "info");
        cJSON *sn = sinfo ? cJSON_GetObjectItem(sinfo, "name") : NULL;
        cJSON *sc = sinfo ? cJSON_GetObjectItem(sinfo, "cover") : NULL;
        if (sn && cJSON_IsString(sn) && sn->valuestring) {
            snprintf(series_name, sizeof series_name, "%s", sn->valuestring);
            evo_title_normalize_spaces(series_name);
        }
        if (sc && cJSON_IsString(sc) && sc->valuestring && strncmp(sc->valuestring, "http", 4) == 0)
            snprintf(series_cover, sizeof series_cover, "%s", sc->valuestring);
        cJSON *sp = sinfo ? cJSON_GetObjectItem(sinfo, "plot") : NULL;
        if (sp && cJSON_IsString(sp) && sp->valuestring)
            snprintf(series_plot, sizeof series_plot, "%s", sp->valuestring);
        cJSON *sy = sinfo ? cJSON_GetObjectItem(sinfo, "releaseDate") : NULL;
        if (!sy || !cJSON_IsString(sy) || !sy->valuestring || !sy->valuestring[0])
            sy = sinfo ? cJSON_GetObjectItem(sinfo, "year") : NULL;
        if (sy && cJSON_IsString(sy) && sy->valuestring)
            snprintf(series_year, sizeof series_year, "%.4s", sy->valuestring);
    }
    char series_id_of_parent[24] = "";
    {
        const char *pid = ctx->parent_id;
        if (strncmp(pid, "series_show:", 12) == 0) snprintf(series_id_of_parent, sizeof series_id_of_parent, "%s", pid + 12);
        else if (strncmp(pid, "series_season:", 14) == 0) {
            snprintf(series_id_of_parent, sizeof series_id_of_parent, "%s", pid + 14);
            char *c = strchr(series_id_of_parent, ':');
            if (c) *c = '\0';
        }
    }
    int target_season = 1;
    if (strncmp(ctx->parent_id, "series_season:", 14) == 0) {
        const char *p = ctx->parent_id + 14;
        const char *colon = strchr(p, ':');
        if (colon) target_season = atoi(colon + 1);
    }

    char snum_key[16];
    snprintf(snum_key, sizeof snum_key, "%d", target_season);
    cJSON *ep_arr = episodes ? cJSON_GetObjectItem(episodes, snum_key) : NULL;
    if (!ep_arr || !cJSON_IsArray(ep_arr)) {
        /* If not found by key, try the first object inside episodes */
        if (episodes && episodes->child && cJSON_IsArray(episodes->child))
            ep_arr = episodes->child;
    }

    if (!ep_arr || !cJSON_IsArray(ep_arr)) {
        if (ctx->cb) ctx->cb(1, NULL, 0, 0, ctx->ud);
        cJSON_Delete(root);
        free(ctx);
        return;
    }

    int count = cJSON_GetArraySize(ep_arr);
    if (count > EVO_PROVIDER_PAGE_MAX) count = EVO_PROVIDER_PAGE_MAX;
    evo_provider_item_t *items = (evo_provider_item_t *)calloc((size_t)count, sizeof *items);
    if (!items) {
        if (ctx->cb) ctx->cb(0, NULL, 0, 0, ctx->ud);
        cJSON_Delete(root);
        free(ctx);
        return;
    }

    for (int i = 0; i < count; ++i) {
        cJSON *ep = cJSON_GetArrayItem(ep_arr, i);
        if (!ep) continue;
        evo_provider_item_clear(&items[i]);

        cJSON *eid = cJSON_GetObjectItem(ep, "id");
        cJSON *title = cJSON_GetObjectItem(ep, "title");
        cJSON *ep_num = cJSON_GetObjectItem(ep, "episode_num");
        cJSON *ext = cJSON_GetObjectItem(ep, "container_extension");
        cJSON *info = cJSON_GetObjectItem(ep, "info");

        char id_buf[32] = {0};
        const char *id_str = eid ? (cJSON_IsString(eid) ? eid->valuestring : "") : "";
        if (!id_str[0] && eid && cJSON_IsNumber(eid)) {
            snprintf(id_buf, sizeof id_buf, "%d", eid->valueint);
            id_str = id_buf;
        }

        const char *ext_str = (ext && ext->valuestring && ext->valuestring[0]) ? ext->valuestring : "mp4";
        int num = ep_num ? (cJSON_IsNumber(ep_num) ? ep_num->valueint : atoi(ep_num->valuestring)) : (i + 1);

        snprintf(items[i].id, sizeof items[i].id, "series_ep:%s:%s:%s", id_str, ext_str, series_id_of_parent);
        snprintf(items[i].parent_id, sizeof items[i].parent_id, "%s", ctx->parent_id);
        /* The title Continue Watching keeps names the series and the episode,
         * so Home groups a series into one tile; the episode's own name goes
         * underneath. Without a series name it reads as before. */
        if (series_name[0]) {
            snprintf(items[i].title, sizeof items[i].title, "%s - S%dE%d", series_name, target_season, num);
            snprintf(items[i].subtitle, sizeof items[i].subtitle, "%s",
                     (title && title->valuestring) ? title->valuestring : "Episode");
            evo_title_normalize_spaces(items[i].subtitle);
        } else {
            snprintf(items[i].title, sizeof items[i].title, "S%02dE%02d - %s",
                     target_season, num, (title && title->valuestring) ? title->valuestring : "Episode");
        }
        if (series_cover[0])
            snprintf(items[i].art_url, sizeof items[i].art_url, "%s", series_cover);
        xt_art_put(items[i].id, items[i].art_url);

        if (info) {
            cJSON *plot = cJSON_GetObjectItem(info, "plot");
            if (plot && plot->valuestring)
                snprintf(items[i].overview, sizeof items[i].overview, "%s", plot->valuestring);
            cJSON *dur = cJSON_GetObjectItem(info, "duration_secs");
            if (dur && cJSON_IsNumber(dur))
                items[i].duration_sec = dur->valueint;
        }
        xt_info_put(items[i].id, series_year, cJSON_GetArraySize(ep_arr),
                    items[i].overview[0] ? items[i].overview : series_plot);

        items[i].kind = EVO_MEDIA_VIDEO;
        items[i].is_live = 0;
        items[i].is_folder = 0;
        xt_fill_watch_state(&items[i]);
    }

    if (ctx->cb) ctx->cb(1, items, count, 0, ctx->ud);
    free(items);
    cJSON_Delete(root);
    free(ctx);
}

/* ------------------------------------------------------------------------- */
/* Home's rows: the newest films and series                                  */
/* ------------------------------------------------------------------------- */
/*
 * Xtream has no "newest" call: the whole film (or series) list comes once,
 * sorted here by when it was added, and the first XT_NEW_COUNT kept as a small
 * JSON array that the ordinary list handler turns into rows. Kept an hour.
 */
#define XT_NEW_COUNT 20
#define XT_NEW_TTL   3600
static char  *g_xt_new[2];          /* 0 films, 1 series */
static time_t g_xt_new_at[2];

typedef struct { double when; int idx; } xt_new_t;

static int xt_new_cmp(const void *a, const void *b)
{
    const double x = ((const xt_new_t *)a)->when, y = ((const xt_new_t *)b)->when;
    return (x < y) ? 1 : (x > y) ? -1 : 0;
}

static double xt_num(const cJSON *o, const char *key)
{
    const cJSON *v = cJSON_GetObjectItem(o, key);
    if (!v) return 0.0;
    if (cJSON_IsNumber(v)) return v->valuedouble;
    if (cJSON_IsString(v) && v->valuestring) return atof(v->valuestring);
    return 0.0;
}

/*
 * The whole film (or series) list also becomes a small index - name, id, year -
 * so Home can look TMDB's titles up here without asking the panel each time.
 */
#define XT_IX_TTL 21600
typedef struct {
    char name[112];
    char id[24];
    char ext[8];
    char year[8];
    char icon[192];                                  /* "" when the URL is longer */
    char cat[16];
} xt_ix_t;
static xt_ix_t *g_xt_ix[2];
static int      g_xt_ix_n[2];
static time_t   g_xt_ix_at[2];

static void xt_str(const cJSON *o, const char *key, char *out, size_t sz)
{
    out[0] = '\0';
    const cJSON *v = cJSON_GetObjectItem(o, key);
    if (!v) return;
    if (cJSON_IsString(v) && v->valuestring) snprintf(out, sz, "%s", v->valuestring);
    else if (cJSON_IsNumber(v))             snprintf(out, sz, "%d", v->valueint);
}

static void xt_index_build(int series, const cJSON *arr, const cJSON **items, int n)
{
    (void)arr;
    xt_ix_t *ix = (xt_ix_t *)calloc((size_t)(n ? n : 1), sizeof *ix);
    if (!ix) return;
    int k = 0;
    for (int i = 0; i < n; ++i) {
        const cJSON *o = items[i];
        xt_ix_t *e = &ix[k];
        xt_str(o, "name", e->name, sizeof e->name);
        xt_str(o, series ? "series_id" : "stream_id", e->id, sizeof e->id);
        if (!e->name[0] || !e->id[0]) continue;
        evo_title_normalize_spaces(e->name);
        if (!series) xt_str(o, "container_extension", e->ext, sizeof e->ext);
        if (!e->ext[0]) snprintf(e->ext, sizeof e->ext, "mp4");
        char y[32];
        xt_str(o, series ? "releaseDate" : "year", y, sizeof y);
        if (!y[0]) xt_str(o, "year", y, sizeof y);
        snprintf(e->year, sizeof e->year, "%.4s", y);
        xt_str(o, "category_id", e->cat, sizeof e->cat);
        char icon[400];
        xt_str(o, series ? "cover" : "stream_icon", icon, sizeof icon);
        if (strncmp(icon, "http", 4) == 0 && strlen(icon) < sizeof e->icon)
            snprintf(e->icon, sizeof e->icon, "%s", icon);
        k++;
    }
    free(g_xt_ix[series]);
    g_xt_ix[series] = ix;
    g_xt_ix_n[series] = k;
    g_xt_ix_at[series] = time(NULL);
}


/* ------------------------------------------------------------------------- */
/* Episodes in order, for L1 / R1 and Up next                                 */
/* ------------------------------------------------------------------------- */
/*
 * Every get_series_info answer is remembered as the series' episodes in
 * order (season, then number). The episode being played is known from
 * resolve, so the one before / after it can be found - into the next season
 * as well. An episode id carries its series ("series_ep:<ep>:<ext>:<series>"),
 * so after a resume from Home the series is fetched when it is not known.
 */
typedef struct { int season, num; char epid[24]; char ext[8]; } xt_ep_t;
typedef struct { char sid[24]; char name[128]; int n; xt_ep_t *eps; time_t used; } xt_series_t;
#define XT_SERIES_SLOTS 16
static xt_series_t g_xt_series[XT_SERIES_SLOTS];
static char g_xt_play_url[EVO_PROVIDER_MAX_URL];
static char g_xt_play_ep[EVO_PROVIDER_MAX_ITEM_ID];
static char g_xt_fetching[24];
static time_t g_xt_fetch_at;

/* "series_ep:<ep>:<ext>[:<series>]" -> its parts. 0 when it is not one. */
static int xt_parse_ep(const char *id, char *ep, size_t ep_sz, char *ext, size_t ext_sz, char *sid, size_t sid_sz)
{
    if (!id || strncmp(id, "series_ep:", 10) != 0) return 0;
    char buf[EVO_PROVIDER_MAX_ITEM_ID];
    snprintf(buf, sizeof buf, "%s", id + 10);
    char *c1 = strchr(buf, ':');
    char *c2 = c1 ? strchr(c1 + 1, ':') : NULL;
    if (c1) *c1 = '\0';
    if (c2) *c2 = '\0';
    snprintf(ep, ep_sz, "%s", buf);
    snprintf(ext, ext_sz, "%s", (c1 && c1[1]) ? c1 + 1 : "mp4");
    snprintf(sid, sid_sz, "%s", (c2 && c2[1]) ? c2 + 1 : "");
    return ep[0] != '\0';
}

static int xt_ep_cmp(const void *a, const void *b)
{
    const xt_ep_t *x = (const xt_ep_t *)a, *y = (const xt_ep_t *)b;
    if (x->season != y->season) return x->season - y->season;
    return x->num - y->num;
}

static void xt_series_remember(const char *sid, const char *name, const cJSON *episodes)
{
    if (!sid || !sid[0] || !episodes || !cJSON_IsObject(episodes)) return;
    int n = 0;
    const cJSON *season;
    cJSON_ArrayForEach(season, episodes)
        if (cJSON_IsArray(season)) n += cJSON_GetArraySize(season);
    if (n <= 0) return;
    xt_ep_t *eps = (xt_ep_t *)calloc((size_t)n, sizeof *eps);
    if (!eps) return;
    int k = 0;
    cJSON_ArrayForEach(season, episodes) {
        if (!cJSON_IsArray(season)) continue;
        const int sn = season->string ? atoi(season->string) : 0;
        int i = 0;
        const cJSON *ep;
        cJSON_ArrayForEach(ep, season) {
            if (k >= n) break;
            xt_ep_t *e = &eps[k];
            xt_str(ep, "id", e->epid, sizeof e->epid);
            if (!e->epid[0]) continue;
            xt_str(ep, "container_extension", e->ext, sizeof e->ext);
            if (!e->ext[0]) snprintf(e->ext, sizeof e->ext, "mp4");
            char num[16];
            xt_str(ep, "episode_num", num, sizeof num);
            e->num = num[0] ? atoi(num) : i + 1;
            char sea[16];
            xt_str(ep, "season", sea, sizeof sea);
            e->season = sea[0] ? atoi(sea) : sn;
            k++;
            i++;
        }
    }
    qsort(eps, (size_t)k, sizeof *eps, xt_ep_cmp);

    xt_series_t *slot = NULL;
    for (int i = 0; i < XT_SERIES_SLOTS && !slot; ++i)
        if (strcmp(g_xt_series[i].sid, sid) == 0) slot = &g_xt_series[i];
    for (int i = 0; i < XT_SERIES_SLOTS && !slot; ++i)
        if (!g_xt_series[i].sid[0]) slot = &g_xt_series[i];
    if (!slot) {                                      /* full: the one used longest ago */
        slot = &g_xt_series[0];
        for (int i = 1; i < XT_SERIES_SLOTS; ++i)
            if (g_xt_series[i].used < slot->used) slot = &g_xt_series[i];
    }
    free(slot->eps);
    memset(slot, 0, sizeof *slot);
    snprintf(slot->sid, sizeof slot->sid, "%s", sid);
    snprintf(slot->name, sizeof slot->name, "%s", name ? name : "");
    slot->eps = eps;
    slot->n = k;
    slot->used = time(NULL);
    if (strcmp(g_xt_fetching, sid) == 0) g_xt_fetching[0] = '\0';
}

static int xt_split_acct(const char *id, const char **rest);
static int g_acct_active;
static void xt_add_prefix(char *id, size_t sz, int acct);

void xtream_note_playing(const char *item_id, const char *url)
{
    xt_split_acct(item_id, &item_id);
    if (!item_id || strncmp(item_id, "series_ep:", 10) != 0 || !url) return;
    snprintf(g_xt_play_ep, sizeof g_xt_play_ep, "%s", item_id);
    snprintf(g_xt_play_url, sizeof g_xt_play_url, "%s", url);
}

int xtream_is_playing_episode(const char *url)
{
    return url && g_xt_play_url[0] && strcmp(url, g_xt_play_url) == 0;
}

static int xt_list_inner(const char *parent_id, int page, evo_provider_items_cb cb, void *ud);

int xtream_episode_neighbor(int dir, char *item_id, size_t id_sz, char *title, size_t title_sz)
{
    char ep[24], ext[8], sid[24];
    if (!xt_parse_ep(g_xt_play_ep, ep, sizeof ep, ext, sizeof ext, sid, sizeof sid)) return -1;
    xt_series_t *se = NULL;
    int at = -1;
    for (int i = 0; i < XT_SERIES_SLOTS && at < 0; ++i) {
        if (!g_xt_series[i].sid[0] || (sid[0] && strcmp(g_xt_series[i].sid, sid) != 0)) continue;
        for (int j = 0; j < g_xt_series[i].n; ++j)
            if (strcmp(g_xt_series[i].eps[j].epid, ep) == 0) { se = &g_xt_series[i]; at = j; break; }
    }
    if (at < 0) {
        if (!sid[0]) return -1;                       /* an old id: its series is not known */
        /* Not read yet (a resume from Home): ask for it, and say "wait". */
        if (strcmp(g_xt_fetching, sid) != 0 || time(NULL) - g_xt_fetch_at > 20) {
            snprintf(g_xt_fetching, sizeof g_xt_fetching, "%s", sid);
            g_xt_fetch_at = time(NULL);
            char parent[48];
            snprintf(parent, sizeof parent, "series_show:%s", sid);
            xt_list_inner(parent, 0, NULL, NULL);
        }
        return -3;
    }
    se->used = time(NULL);
    const int to = at + (dir > 0 ? 1 : -1);
    if (to < 0 || to >= se->n) return -1;
    const xt_ep_t *e = &se->eps[to];
    snprintf(item_id, id_sz, "series_ep:%s:%s:%s", e->epid, e->ext, se->sid);
    xt_add_prefix(item_id, id_sz, g_acct_active);
    if (se->name[0]) snprintf(title, title_sz, "%s - S%dE%d", se->name, e->season, e->num);
    else             snprintf(title, title_sz, "S%dE%d", e->season, e->num);
    return 0;
}

/* "@find:v:<text>": every indexed title with <text> in its name. */
static void xt_find_emit(int series, const char *text, evo_provider_items_cb cb, void *ud)
{
    evo_provider_item_t *items = (evo_provider_item_t *)calloc(EVO_PROVIDER_PAGE_MAX, sizeof *items);
    if (!items) { if (cb) cb(0, NULL, 0, 0, ud); return; }
    int k = 0;
    for (int i = 0; i < g_xt_ix_n[series] && k < EVO_PROVIDER_PAGE_MAX; ++i) {
        const xt_ix_t *e = &g_xt_ix[series][i];
        if (!xt_ci_contains(e->name, text)) continue;
        {
            char key[48];
            snprintf(key, sizeof key, "%s%s", series ? "series_cat:" : "vod_cat:", e->cat);
            if (evo_parental_hides("xtream", e->cat[0] ? key : "", e->name)) continue;
        }
        evo_provider_item_t *it = &items[k++];
        evo_provider_item_clear(it);
        if (series) {
            snprintf(it->id, sizeof it->id, "series_show:%s", e->id);
            it->kind = EVO_MEDIA_FOLDER;
            it->is_folder = 1;
        } else {
            snprintf(it->id, sizeof it->id, "vod_stream:%s:%s", e->id, e->ext);
            it->kind = EVO_MEDIA_VIDEO;
        }
        snprintf(it->title, sizeof it->title, "%s", e->name);
        snprintf(it->subtitle, sizeof it->subtitle, "%s", e->year);
        snprintf(it->art_url, sizeof it->art_url, "%s", e->icon);
        if (!series) xt_fill_watch_state(it);
    }
    if (cb) cb(1, items, k, 0, ud);
    free(items);
}


/* ------------------------------------------------------------------------- */
/* TV guide: "xg:<stream id>"                                                */
/* ------------------------------------------------------------------------- */
/*
 * The channel's programmes from get_simple_data_table, from earlier today to
 * as far ahead as the panel knows, each line "Mon 13  20:00  Title". What is
 * on now plays the channel; the rest says when it is on.
 */
static int b64_val(unsigned char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+' || c == '-') return 62;
    if (c == '/' || c == '_') return 63;
    return -1;
}

/* Base64 (what Xtream wraps EPG text in) into out; plain text stays as it is. */
static void xt_b64(const char *in, char *out, size_t sz)
{
    size_t k = 0;
    int bits = 0, acc = 0, bad = 0;
    for (const unsigned char *c = (const unsigned char *)in; *c && k + 1 < sz; ++c) {
        if (*c == '=' || *c == '\n' || *c == '\r') continue;
        const int v = b64_val(*c);
        if (v < 0) { bad = 1; break; }
        acc = (acc << 6) | v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out[k++] = (char)((acc >> bits) & 0xFF);
        }
    }
    out[k] = '\0';
    /* Not base64 after all ("News" decodes, to bytes nobody can read). */
    for (size_t i = 0; !bad && i < k; ++i)
        if ((unsigned char)out[i] < 0x20 && out[i] != '\n' && out[i] != '\t') bad = 1;
    if (bad) snprintf(out, sz, "%s", in);
}

typedef struct {
    evo_provider_items_cb cb;
    void *ud;
    char sid[24];
    char parent[EVO_PROVIDER_MAX_ITEM_ID];
} guide_ctx_t;

static void on_guide_resp(int success, int status, const char *body, size_t len, void *ud)
{
    (void)len;
    guide_ctx_t *g = (guide_ctx_t *)ud;
    cJSON *root = (success && status >= 200 && status < 300 && body) ? cJSON_Parse(body) : NULL;
    const cJSON *list = root ? cJSON_GetObjectItem(root, "epg_listings") : NULL;
    if (!list || !cJSON_IsArray(list)) {
        if (root) cJSON_Delete(root);
        if (g->cb) g->cb(0, NULL, 0, 0, g->ud);
        free(g);
        return;
    }
    evo_provider_item_t *items = (evo_provider_item_t *)calloc(EVO_PROVIDER_PAGE_MAX, sizeof *items);
    if (!items) {
        cJSON_Delete(root);
        if (g->cb) g->cb(0, NULL, 0, 0, g->ud);
        free(g);
        return;
    }
    static const char *wd[7] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
    const long now = (long)time(NULL);
    struct tm today;
    { time_t t = (time_t)now; localtime_r(&t, &today); }
    int k = 0;
    const cJSON *o;
    /* Catch-up: how far back this channel's archive goes. */
    int arch_days = 0;
    {
        const xt_ch_t *ch = xt_ch_slot(g->sid, 0);
        if (ch) arch_days = ch->archive_days;
    }
    long keep_from = now - 6 * 3600;
    if (arch_days > 0) keep_from = now - (long)arch_days * 86400;
    /* Room for what is on next: the oldest archive is dropped first. */
    int total = 0, skip = 0;
    cJSON_ArrayForEach(o, list) {
        char te[24];
        xt_str(o, "stop_timestamp", te, sizeof te);
        const long b = atol(te);
        if (!(b > 0 && b < keep_from)) total++;
    }
    if (total > EVO_PROVIDER_PAGE_MAX) skip = total - EVO_PROVIDER_PAGE_MAX;
    cJSON_ArrayForEach(o, list) {
        if (k >= EVO_PROVIDER_PAGE_MAX) break;
        char ts[24], te[24], raw[512], title[192];
        xt_str(o, "start_timestamp", ts, sizeof ts);
        xt_str(o, "stop_timestamp", te, sizeof te);
        const long a = atol(ts), b = atol(te);
        if (b > 0 && b < keep_from) continue;          /* long gone */
        if (skip > 0) { skip--; continue; }
        if (!g_xt_tz_known) {
            char civil[32];
            xt_str(o, "start", civil, sizeof civil);
            xt_note_clock(civil, a);
        }
        const int has_arch = arch_days > 0 || (int)xt_num(o, "has_archive") != 0;
        xt_str(o, "title", raw, sizeof raw);
        xt_b64(raw, title, sizeof title);
        evo_title_normalize_spaces(title);
        if (!title[0]) continue;
        evo_provider_item_t *it = &items[k++];
        evo_provider_item_clear(it);
        snprintf(it->parent_id, sizeof it->parent_id, "%s", g->parent);
        time_t t = (time_t)a;
        struct tm tmv;
        localtime_r(&t, &tmv);
        char day[16] = "";
        if (tmv.tm_yday != today.tm_yday || tmv.tm_year != today.tm_year)
            snprintf(day, sizeof day, "%s %02d  ", wd[tmv.tm_wday % 7], tmv.tm_mday);
        const int on_now = a > 0 && b > 0 && a <= now && now < b;
        if (on_now) {
            snprintf(it->id, sizeof it->id, "live_stream:%s", g->sid);
            snprintf(it->title, sizeof it->title, "NOW  %02d:%02d  %s", tmv.tm_hour, tmv.tm_min, title);
            snprintf(it->subtitle, sizeof it->subtitle, "On now - press to watch");
            it->is_live = 1;
            it->kind = EVO_MEDIA_STREAM;
        } else if (has_arch && a > 0 && b > a && b <= now) {
            snprintf(it->id, sizeof it->id, "xa:%s:%ld:%ld", g->sid, a, b);
            snprintf(it->title, sizeof it->title, "%s%02d:%02d  %s", day, tmv.tm_hour, tmv.tm_min, title);
            snprintf(it->subtitle, sizeof it->subtitle, "Catch-up - press to watch");
            it->kind = EVO_MEDIA_VIDEO;
        } else {
            snprintf(it->id, sizeof it->id, "xi:%s:%ld:%s", g->sid, a, (b > 0 && b <= now) ? "past" : "later");
            snprintf(it->title, sizeof it->title, "%s%02d:%02d  %s", day, tmv.tm_hour, tmv.tm_min, title);
            snprintf(it->subtitle, sizeof it->subtitle, "TV guide");
            it->kind = EVO_MEDIA_VIDEO;
        }
        xt_str(o, "description", raw, sizeof raw);
        xt_b64(raw, it->overview, sizeof it->overview);
    }
    cJSON_Delete(root);
    if (g->cb) g->cb(1, items, k, 0, g->ud);
    free(items);
    free(g);
}

/* A whole film (or series) list: the index for lookups and the newest few
 * for Home. 0 when the body is not a list. */
static int xt_ingest_full(int series, const char *body)
{
    cJSON *arr = body ? cJSON_Parse(body) : NULL;
    if (!arr || !cJSON_IsArray(arr)) {
        if (arr) cJSON_Delete(arr);
        return 0;
    }
    /* Walk the list once: GetArrayItem(i) on a long list starts from the
     * front every time. */
    int n = 0;
    const cJSON *el;
    cJSON_ArrayForEach(el, arr) n++;
    const cJSON **all = (const cJSON **)calloc((size_t)(n ? n : 1), sizeof *all);
    xt_new_t *order = (xt_new_t *)calloc((size_t)(n ? n : 1), sizeof *order);
    if (all && order) {
        int i = 0;
        cJSON_ArrayForEach(el, arr) all[i++] = el;
        xt_index_build(series, arr, all, n);
        for (i = 0; i < n; ++i) {
            order[i].idx = i;
            order[i].when = xt_num(all[i], series ? "last_modified" : "added");
        }
        qsort(order, (size_t)n, sizeof *order, xt_new_cmp);
        /* The first XT_NEW_COUNT, printed one by one into a small array. */
        size_t cap = 4096, used = 0;
        char *out = (char *)malloc(cap);
        if (out) out[used++] = '[';
        for (i = 0; out && i < n && i < XT_NEW_COUNT; ++i) {
            char *one = cJSON_PrintUnformatted(all[order[i].idx]);
            if (!one) continue;
            const size_t len1 = strlen(one);
            if (used + len1 + 3 > cap) {
                while (used + len1 + 3 > cap) cap *= 2;
                char *grown = (char *)realloc(out, cap);
                if (!grown) { free(one); free(out); out = NULL; break; }
                out = grown;
            }
            if (used > 1) out[used++] = ',';
            memcpy(out + used, one, len1);
            used += len1;
            free(one);
        }
        if (out) {
            out[used++] = ']';
            out[used] = '\0';
            free(g_xt_new[series]);
            g_xt_new[series] = out;
            g_xt_new_at[series] = time(NULL);
        }
    }
    free(all);
    free(order);
    cJSON_Delete(arr);
    return 1;
}

static void on_new_resp(int success, int status, const char *body, size_t len, void *ud)
{
    (void)len;
    catalog_ctx_t *ctx = (catalog_ctx_t *)ud;
    if (!ctx) return;
    const int find = strncmp(ctx->parent_id, "@find:", 6) == 0;
    const int series = (find ? ctx->parent_id[6] : ctx->parent_id[5]) == 's';
    if (!success || status < 200 || status >= 300 || !xt_ingest_full(series, body)) {
        if (ctx->cb) ctx->cb(0, NULL, 0, 0, ctx->ud);
        free(ctx);
        return;
    }
    if (find) {
        xt_find_emit(series, ctx->parent_id + 8, ctx->cb, ctx->ud);
        free(ctx);
        return;
    }
    if (!g_xt_new[series]) {
        if (ctx->cb) ctx->cb(0, NULL, 0, 0, ctx->ud);
        free(ctx);
        return;
    }
    const char *kept = g_xt_new[series];
    if (series) on_series_resp(1, 200, kept, strlen(kept), ctx);
    else        on_vod_streams_resp(1, 200, kept, strlen(kept), ctx);
}

static int xt_list_inner(const char *parent_id, int page,
                               evo_provider_items_cb cb, void *ud)
{
    (void)page;
    if (!g_cfg.host[0] || !g_cfg.username[0] || !g_cfg.password[0]) {
        if (cb) cb(0, NULL, 0, 0, ud);
        return -1;
    }

    /* 1. Root level: 3 distinct catalogs */
    if (!parent_id || !*parent_id) {
        evo_provider_item_t root_items[3];
        memset(root_items, 0, sizeof root_items);

        snprintf(root_items[0].id, sizeof root_items[0].id, "live");
        snprintf(root_items[0].title, sizeof root_items[0].title, "Live Channels");
        snprintf(root_items[0].subtitle, sizeof root_items[0].subtitle, "Live broadcast TV streams");
        root_items[0].kind = EVO_MEDIA_FOLDER;
        root_items[0].is_folder = 1;

        snprintf(root_items[1].id, sizeof root_items[1].id, "vod");
        snprintf(root_items[1].title, sizeof root_items[1].title, "Movies (VOD)");
        snprintf(root_items[1].subtitle, sizeof root_items[1].subtitle, "Feature films on demand");
        root_items[1].kind = EVO_MEDIA_FOLDER;
        root_items[1].is_folder = 1;

        snprintf(root_items[2].id, sizeof root_items[2].id, "series");
        snprintf(root_items[2].title, sizeof root_items[2].title, "TV Series");
        snprintf(root_items[2].subtitle, sizeof root_items[2].subtitle, "Television series & episodes");
        root_items[2].kind = EVO_MEDIA_FOLDER;
        root_items[2].is_folder = 1;

        if (cb) cb(1, root_items, 3, 0, ud);
        return 0;
    }

    catalog_ctx_t *ctx = (catalog_ctx_t *)calloc(1, sizeof *ctx);
    if (!ctx) return -2;
    ctx->cb = cb;
    ctx->ud = ud;
    snprintf(ctx->parent_id, sizeof ctx->parent_id, "%s", parent_id);

    char url[1024];

    /* 2. Live categories */
    if (strcmp(parent_id, "live") == 0) {
        build_api_url(url, sizeof url, "action=get_live_categories");
        return xt_get_cached(url, on_categories_resp, ctx);
    }

    /* 3. Live streams in category */
    if (strncmp(parent_id, "live_cat:", 9) == 0) {
        char q[64];
        snprintf(q, sizeof q, "action=get_live_streams&category_id=%s", parent_id + 9);
        {
            char gu[EVO_EPG_MAX_URL];
            build_xmltv_url(gu, sizeof gu);
            evo_epg_set_provider_source("xtream", gu);
        }
        build_api_url(url, sizeof url, q);
        return xt_get_cached(url, on_live_streams_resp, ctx);
    }

    /* 4. VOD categories */
    if (strcmp(parent_id, "vod") == 0) {
        build_api_url(url, sizeof url, "action=get_vod_categories");
        return xt_get_cached(url, on_categories_resp, ctx);
    }

    /* 5. VOD streams in category */
    if (strncmp(parent_id, "vod_cat:", 8) == 0) {
        char q[64];
        snprintf(q, sizeof q, "action=get_vod_streams&category_id=%s", parent_id + 8);
        build_api_url(url, sizeof url, q);
        return xt_get_cached(url, on_vod_streams_resp, ctx);
    }

    /* 6. Series categories */
    if (strcmp(parent_id, "series") == 0) {
        build_api_url(url, sizeof url, "action=get_series_categories");
        return xt_get_cached(url, on_categories_resp, ctx);
    }

    /* 7. Series list in category */
    if (strncmp(parent_id, "series_cat:", 11) == 0) {
        char q[64];
        snprintf(q, sizeof q, "action=get_series&category_id=%s", parent_id + 11);
        build_api_url(url, sizeof url, q);
        return xt_get_cached(url, on_series_resp, ctx);
    }

    /* 8. Series info (seasons & episodes) */
    if (strncmp(parent_id, "series_show:", 12) == 0) {
        char q[64];
        snprintf(q, sizeof q, "action=get_series_info&series_id=%s", parent_id + 12);
        build_api_url(url, sizeof url, q);
        return xt_get_cached(url, on_series_info_resp, ctx);
    }

    /* TV guide for one channel: not kept, it changes by the hour */
    if (strncmp(parent_id, "xg:", 3) == 0 && parent_id[3]) {
        guide_ctx_t *g = (guide_ctx_t *)calloc(1, sizeof *g);
        if (!g) { free(ctx); return -2; }
        g->cb = cb;
        g->ud = ud;
        snprintf(g->sid, sizeof g->sid, "%s", parent_id + 3);
        snprintf(g->parent, sizeof g->parent, "%s", parent_id);
        free(ctx);
        char q[96];
        snprintf(q, sizeof q, "action=get_simple_data_table&stream_id=%s", g->sid);
        build_api_url(url, sizeof url, q);
        const int rc = evo_net_request_async("GET", url, NULL, NULL, 0, on_guide_resp, g);
        if (rc != 0) free(g);
        return rc;
    }

    /* Home: TMDB's titles looked up in this account */
    if (strncmp(parent_id, "@find:", 6) == 0 && (parent_id[6] == 'v' || parent_id[6] == 's') &&
        parent_id[7] == ':') {
        const int series = parent_id[6] == 's';
        if (g_xt_ix[series] && time(NULL) - g_xt_ix_at[series] < XT_IX_TTL) {
            xt_find_emit(series, parent_id + 8, cb, ud);
            free(ctx);
            return 0;
        }
        build_api_url(url, sizeof url, series ? "action=get_series" : "action=get_vod_streams");
        return xt_get_cached(url, on_new_resp, ctx);
    }

    /* Home's rows */
    if (strncmp(parent_id, "@new:", 5) == 0) {
        const int series = parent_id[5] == 's';
        if (g_xt_new[series] && time(NULL) - g_xt_new_at[series] < XT_NEW_TTL) {
            const char *kept = g_xt_new[series];
            if (series) on_series_resp(1, 200, kept, strlen(kept), ctx);
            else        on_vod_streams_resp(1, 200, kept, strlen(kept), ctx);
            return 0;
        }
        build_api_url(url, sizeof url, series ? "action=get_series" : "action=get_vod_streams");
        return xt_get_cached(url, on_new_resp, ctx);
    }

    if (strncmp(parent_id, "series_season:", 14) == 0) {
        const char *p = parent_id + 14;
        const char *colon = strchr(p, ':');
        char series_id[32] = {0};
        if (colon) {
            size_t n = (size_t)(colon - p);
            if (n >= sizeof series_id) n = sizeof series_id - 1;
            memcpy(series_id, p, n);
        } else {
            snprintf(series_id, sizeof series_id, "%s", p);
        }
        char q[64];
        snprintf(q, sizeof q, "action=get_series_info&series_id=%s", series_id);
        build_api_url(url, sizeof url, q);
        return xt_get_cached(url, on_series_info_resp, ctx);
    }

    free(ctx);
    if (cb) cb(0, NULL, 0, 0, ud);
    return -1;
}

/* ------------------------------------------------------------------------- */
/* Stream Resolution & Multi-Format Selection                                */
/* ------------------------------------------------------------------------- */

static int xt_resolve_inner(const char *item_id, evo_provider_resolve_cb cb, void *ud)
{
    if (!item_id || !g_cfg.host[0] || !g_cfg.username[0] || !g_cfg.password[0])
        return -1;

    const char *scheme = g_cfg.use_https ? "https" : "http";
    int default_port = g_cfg.use_https ? 443 : 80;
    char host_port[160];
    if (g_cfg.port > 0 && g_cfg.port != default_port)
        snprintf(host_port, sizeof host_port, "%s:%d", g_cfg.host, g_cfg.port);
    else
        snprintf(host_port, sizeof host_port, "%s", g_cfg.host);

    /* 1. Live stream resolution -> Offers both HLS (.m3u8) and MPEG-TS (.ts) */
    if (strncmp(item_id, "live_stream:", 12) == 0) {
        const char *stream_id = item_id + 12;
        evo_stream_choice_t choices[2];
        memset(choices, 0, sizeof choices);

        bool prefer_ts = (strcmp(g_cfg.stream_format, "ts") == 0);

        const char *ext0 = prefer_ts ? "ts" : "m3u8";
        const char *lbl0 = prefer_ts ? "MPEG-TS (Raw Stream)" : "HLS (Adaptive Stream)";
        const char *ext1 = prefer_ts ? "m3u8" : "ts";
        const char *lbl1 = prefer_ts ? "HLS (Adaptive Stream)" : "MPEG-TS (Raw Stream)";

        snprintf(choices[0].url, sizeof choices[0].url,
                 "%s://%s/live/%s/%s/%s.%s",
                 scheme, host_port, g_cfg.username, g_cfg.password, stream_id, ext0);
        snprintf(choices[0].label, sizeof choices[0].label, "%s", lbl0);
        snprintf(choices[0].container, sizeof choices[0].container, "%s", ext0);
        choices[0].is_live = 1;

        snprintf(choices[1].url, sizeof choices[1].url,
                 "%s://%s/live/%s/%s/%s.%s",
                 scheme, host_port, g_cfg.username, g_cfg.password, stream_id, ext1);
        snprintf(choices[1].label, sizeof choices[1].label, "%s", lbl1);
        snprintf(choices[1].container, sizeof choices[1].container, "%s", ext1);
        choices[1].is_live = 1;

        char masked[512];
        mask_url(choices[0].url, masked, sizeof masked);
        PROV_LOG("xtream: resolved live stream to %s (format: %s)", masked, ext0);

        if (cb) cb(1, choices, 2, ud);
        return 0;
    }

    /* Catch-up: "xa:<sid>:<start>:<stop>" -> /timeshift/u/p/<minutes>/<Y-m-d:H-M>/<sid>.ts,
     * the start in the panel's own clock. */
    if (strncmp(item_id, "xa:", 3) == 0) {
        char sid[24] = "";
        long a = 0, b = 0;
        if (sscanf(item_id + 3, "%23[^:]:%ld:%ld", sid, &a, &b) != 3 || !sid[0] || a <= 0 || b <= a)
            return -1;
        long off = g_xt_tz_off;
        if (!g_xt_tz_known) {
            struct tm lt;
            time_t t = (time_t)a;
            localtime_r(&t, &lt);
            off = (long)lt.tm_gmtoff;
        }
        time_t shifted = (time_t)(a + off);
        struct tm st;
        gmtime_r(&shifted, &st);
        char when[32];
        snprintf(when, sizeof when, "%04d-%02d-%02d:%02d-%02d", st.tm_year + 1900, st.tm_mon + 1, st.tm_mday,
                 st.tm_hour, st.tm_min);
        const long minutes = (b - a + 59) / 60;

        evo_stream_choice_t choices[2];
        memset(choices, 0, sizeof choices);
        snprintf(choices[0].url, sizeof choices[0].url, "%s://%s/timeshift/%s/%s/%ld/%s/%s.ts",
                 scheme, host_port, g_cfg.username, g_cfg.password, minutes, when, sid);
        snprintf(choices[0].label, sizeof choices[0].label, "Catch-up");
        snprintf(choices[0].container, sizeof choices[0].container, "ts");
        choices[0].is_live = 1;
        snprintf(choices[1].url, sizeof choices[1].url,
                 "%s://%s/streaming/timeshift.php?username=%s&password=%s&stream=%s&start=%s&duration=%ld",
                 scheme, host_port, g_cfg.username, g_cfg.password, sid, when, minutes);
        snprintf(choices[1].label, sizeof choices[1].label, "Catch-up (alternative)");
        snprintf(choices[1].container, sizeof choices[1].container, "ts");
        choices[1].is_live = 1;
        PROV_LOG("xtream: catch-up %s from %s for %ld min (offset %ld s%s)",
                 sid, when, minutes, off, g_xt_tz_known ? "" : ", console clock");
        if (cb) cb(1, choices, 2, ud);
        return 0;
    }

    /* 2. VOD movie resolution -> /movie/u/p/id.ext */
    if (strncmp(item_id, "vod_stream:", 11) == 0) {
        const char *p = item_id + 11;
        const char *colon = strchr(p, ':');
        char stream_id[32] = {0};
        const char *ext = "mp4";
        if (colon) {
            size_t n = (size_t)(colon - p);
            if (n >= sizeof stream_id) n = sizeof stream_id - 1;
            memcpy(stream_id, p, n);
            ext = colon + 1;
        } else {
            snprintf(stream_id, sizeof stream_id, "%s", p);
        }

        evo_stream_choice_t choice;
        memset(&choice, 0, sizeof choice);
        snprintf(choice.url, sizeof choice.url,
                 "%s://%s/movie/%s/%s/%s.%s",
                 scheme, host_port, g_cfg.username, g_cfg.password, stream_id, ext);
        snprintf(choice.label, sizeof choice.label, "Direct (Movie)");
        choice.max_connections = 1;   /* one stream per line on an Xtream panel */
        xt_choice_info(item_id, &choice);
        snprintf(choice.container, sizeof choice.container, "%s", ext);
        choice.is_live = 0;

        char masked[512];
        mask_url(choice.url, masked, sizeof masked);
        PROV_LOG("xtream: resolved VOD to %s", masked);

        if (cb) cb(1, &choice, 1, ud);
        return 0;
    }

    /* 3. Series episode resolution -> /series/u/p/id.ext */
    if (strncmp(item_id, "series_ep:", 10) == 0) {
        char episode_id[32] = {0}, ext[8] = "mp4", sid[24] = "";
        xt_parse_ep(item_id, episode_id, sizeof episode_id, ext, sizeof ext, sid, sizeof sid);

        evo_stream_choice_t choice;
        memset(&choice, 0, sizeof choice);
        snprintf(choice.url, sizeof choice.url,
                 "%s://%s/series/%s/%s/%s.%s",
                 scheme, host_port, g_cfg.username, g_cfg.password, episode_id, ext);
        snprintf(choice.label, sizeof choice.label, "Direct (Series)");
        choice.max_connections = 1;
        xt_choice_info(item_id, &choice);
        snprintf(choice.container, sizeof choice.container, "%s", ext);
        choice.is_live = 0;

        char masked[512];
        mask_url(choice.url, masked, sizeof masked);
        PROV_LOG("xtream: resolved episode to %s", masked);
        /* What plays now, for L1 / R1 and Up next. */
        snprintf(g_xt_play_url, sizeof g_xt_play_url, "%s", choice.url);
        snprintf(g_xt_play_ep, sizeof g_xt_play_ep, "%s", item_id);

        if (cb) cb(1, &choice, 1, ud);
        return 0;
    }

    return -1;
}

/* ------------------------------------------------------------------------- */
/* Search                                                                    */
/* ------------------------------------------------------------------------- */

/*
 * Search: films and series first (from the title index, fetched once when it
 * is missing or old), then live channels (the panel's channel list).
 */
typedef struct {
    evo_provider_items_cb cb;
    void *ud;
    char query[64];
    int  stage;                       /* 0 films, 1 series, 2 channels */
    evo_provider_item_t *acc;
    int  n;
} search_ctx_t;

static void search_step(search_ctx_t *ctx);

static void search_finish(search_ctx_t *ctx, int ok)
{
    if (ctx->cb) ctx->cb(ok, ctx->acc, ctx->n, 0, ctx->ud);
    free(ctx->acc);
    free(ctx);
}

static void search_take(int ok, const evo_provider_item_t *items, int count, int more, void *ud)
{
    (void)more;
    search_ctx_t *ctx = (search_ctx_t *)ud;
    for (int i = 0; ok && items && i < count && ctx->n < EVO_PROVIDER_PAGE_MAX; ++i)
        ctx->acc[ctx->n++] = items[i];
}

static void on_search_index(int success, int status, const char *body, size_t len, void *ud)
{
    (void)len;
    search_ctx_t *ctx = (search_ctx_t *)ud;
    if (success && status >= 200 && status < 300) xt_ingest_full(ctx->stage == 1, body);
    if (g_xt_ix[ctx->stage == 1]) xt_find_emit(ctx->stage == 1, ctx->query, search_take, ctx);
    ctx->stage++;
    search_step(ctx);
}

static void on_search_resp(int success, int status, const char *body, size_t len, void *ud)
{
    (void)len;
    search_ctx_t *ctx = (search_ctx_t *)ud;
    if (!ctx) return;
    cJSON *arr = (success && status >= 200 && status < 300 && body) ? cJSON_Parse(body) : NULL;
    if (arr && cJSON_IsArray(arr)) {
        const cJSON *obj;
        cJSON_ArrayForEach(obj, arr) {
            if (ctx->n >= EVO_PROVIDER_PAGE_MAX) break;
            cJSON *name = cJSON_GetObjectItem(obj, "name");
            if (!name || !name->valuestring || !xt_ci_contains(name->valuestring, ctx->query)) continue;
            if (xt_hidden(obj, "live_cat:", name->valuestring)) continue;
            char id_str[32];
            xt_str(obj, "stream_id", id_str, sizeof id_str);
            cJSON *icon = cJSON_GetObjectItem(obj, "stream_icon");
            evo_provider_item_t *it = &ctx->acc[ctx->n++];
            evo_provider_item_clear(it);
            snprintf(it->id, sizeof it->id, "live_stream:%s", id_str);
            snprintf(it->title, sizeof it->title, "%s", name->valuestring);
            evo_title_normalize_spaces(it->title);
            if (icon && icon->valuestring && icon->valuestring[0])
                snprintf(it->art_url, sizeof it->art_url, "%s", icon->valuestring);
            it->kind = EVO_MEDIA_STREAM;
            it->is_live = 1;
        }
    }
    if (arr) cJSON_Delete(arr);
    search_finish(ctx, 1);                /* what films and series gave counts too */
}

static void search_step(search_ctx_t *ctx)
{
    char url[1024];
    while (ctx->stage < 2) {
        const int series = ctx->stage == 1;
        if (g_xt_ix[series] && time(NULL) - g_xt_ix_at[series] < XT_IX_TTL) {
            xt_find_emit(series, ctx->query, search_take, ctx);
            ctx->stage++;
            continue;
        }
        build_api_url(url, sizeof url, series ? "action=get_series" : "action=get_vod_streams");
        if (xt_get_cached(url, on_search_index, ctx) == 0) return;
        ctx->stage++;
    }
    build_api_url(url, sizeof url, "action=get_live_streams");
    if (xt_get_cached(url, on_search_resp, ctx) != 0)
        search_finish(ctx, ctx->n > 0);
}

static int xt_search_inner(const char *query, int page, evo_provider_items_cb cb, void *ud)
{
    (void)page;
    if (!query || !*query || !g_cfg.host[0] || !g_cfg.username[0] || !g_cfg.password[0]) {
        if (cb) cb(0, NULL, 0, 0, ud);
        return -1;
    }
    search_ctx_t *ctx = (search_ctx_t *)calloc(1, sizeof *ctx);
    if (!ctx) return -2;
    ctx->acc = (evo_provider_item_t *)calloc(EVO_PROVIDER_PAGE_MAX, sizeof *ctx->acc);
    if (!ctx->acc) { free(ctx); return -2; }
    ctx->cb = cb;
    ctx->ud = ud;
    snprintf(ctx->query, sizeof ctx->query, "%s", query);
    search_step(ctx);
    return 0;
}

/* ------------------------------------------------------------------------- */
/* Several accounts                                                          */
/* ------------------------------------------------------------------------- */
/*
 * xtream.conf keeps the account in use, as before; xtream_accounts.conf keeps
 * them all. Every account has a number that never changes. The first one (the
 * account there was before accounts existed) is 0 and its ids look as they
 * always did; any other account's ids start with "<number>|", so a favourite
 * or a Continue entry still opens on the account it came from.
 */
#define XTREAM_ACCTS   "xtream_accounts.conf"
#define XT_MAX_ACCTS   8
typedef struct {
    int  id;
    char host[128];
    int  port;
    char user[64];
    char pass[64];
    int  https;
    char fmt[16];
} xt_acct_t;
static xt_acct_t g_acct[XT_MAX_ACCTS];
static int g_acct_n;
static int g_acct_next;
static int g_acct_active = -1;

static void xt_accts_save(void)
{
    FILE *f = fopen(evo_data_path(XTREAM_ACCTS), "w");
    if (!f) return;
    fprintf(f, "# Kijkr - Xtream accounts\nnext=%d\nactive=%d\n", g_acct_next, g_acct_active);
    for (int i = 0; i < g_acct_n; ++i) {
        const xt_acct_t *a = &g_acct[i];
        fprintf(f, "acct=%d\t%s\t%d\t%s\t%s\t%d\t%s\n", a->id, a->host, a->port, a->user, a->pass,
                a->https, a->fmt[0] ? a->fmt : "m3u8");
    }
    fclose(f);
}

static xt_acct_t *xt_acct_by_id(int id)
{
    for (int i = 0; i < g_acct_n; ++i)
        if (g_acct[i].id == id) return &g_acct[i];
    return NULL;
}

static void xt_acct_from_cfg(xt_acct_t *a)
{
    snprintf(a->host, sizeof a->host, "%s", g_cfg.host);
    a->port = g_cfg.port;
    snprintf(a->user, sizeof a->user, "%s", g_cfg.username);
    snprintf(a->pass, sizeof a->pass, "%s", g_cfg.password);
    a->https = g_cfg.use_https ? 1 : 0;
    snprintf(a->fmt, sizeof a->fmt, "%s", g_cfg.stream_format[0] ? g_cfg.stream_format : "m3u8");
}

/* Splits a tab-separated line in place; empty fields are kept. */
static int xt_split_tabs(char *s, char **out, int max)
{
    int n = 0;
    while (n < max) {
        out[n++] = s;
        char *t = strchr(s, '\t');
        if (!t) break;
        *t = 0;
        s = t + 1;
    }
    return n;
}

static void xt_accts_load(void)
{
    g_acct_n = 0;
    g_acct_next = 0;
    g_acct_active = -1;
    FILE *f = fopen(evo_data_path(XTREAM_ACCTS), "r");
    if (f) {
        char line[768];
        while (fgets(line, sizeof line, f)) {
            size_t n = strlen(line);
            while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
            if (strncmp(line, "next=", 5) == 0)        g_acct_next = atoi(line + 5);
            else if (strncmp(line, "active=", 7) == 0) g_acct_active = atoi(line + 7);
            else if (strncmp(line, "acct=", 5) == 0 && g_acct_n < XT_MAX_ACCTS) {
                char *fld[7];
                const int nf = xt_split_tabs(line + 5, fld, 7);
                if (nf < 6) continue;
                xt_acct_t *a = &g_acct[g_acct_n];
                memset(a, 0, sizeof *a);
                a->id = atoi(fld[0]);
                snprintf(a->host, sizeof a->host, "%s", fld[1]);
                a->port = atoi(fld[2]);
                snprintf(a->user, sizeof a->user, "%s", fld[3]);
                snprintf(a->pass, sizeof a->pass, "%s", fld[4]);
                a->https = atoi(fld[5]);
                snprintf(a->fmt, sizeof a->fmt, "%s", (nf > 6 && fld[6][0]) ? fld[6] : "m3u8");
                if (!a->host[0] || xt_acct_by_id(a->id)) continue;
                if (a->id >= g_acct_next) g_acct_next = a->id + 1;
                g_acct_n++;
            }
        }
        fclose(f);
    }
    if (g_acct_n == 0) {
        /* Before accounts: the one in xtream.conf becomes account 0. */
        g_acct_next = 0;
        g_acct_active = -1;
        if (g_cfg.host[0]) {
            memset(&g_acct[0], 0, sizeof g_acct[0]);
            g_acct[0].id = g_acct_next++;
            xt_acct_from_cfg(&g_acct[0]);
            g_acct_n = 1;
            g_acct_active = g_acct[0].id;
            xt_accts_save();
        }
        return;
    }
    /* xtream.conf has the last word on the account in use. */
    xt_acct_t *act = xt_acct_by_id(g_acct_active);
    if (!act) {
        g_acct_active = g_acct[0].id;
        act = &g_acct[0];
    }
    if (g_cfg.host[0] && strcmp(g_cfg.host, act->host) == 0 && strcmp(g_cfg.username, act->user) == 0) {
        xt_acct_from_cfg(act);
    } else {
        memset(&g_cfg, 0, sizeof g_cfg);
        snprintf(g_cfg.host, sizeof g_cfg.host, "%s", act->host);
        g_cfg.port = act->port;
        snprintf(g_cfg.username, sizeof g_cfg.username, "%s", act->user);
        snprintf(g_cfg.password, sizeof g_cfg.password, "%s", act->pass);
        g_cfg.use_https = act->https != 0;
        snprintf(g_cfg.stream_format, sizeof g_cfg.stream_format, "%s", act->fmt);
    }
    PROV_LOG("xtream: %d account(s), using %d", g_acct_n, g_acct_active);
}

/* After xtream.conf was written: the account in use follows it. */
static void xt_accts_sync(void)
{
    if (!g_cfg.host[0]) return;
    xt_acct_t *act = xt_acct_by_id(g_acct_active);
    if (!act) {
        if (g_acct_n >= XT_MAX_ACCTS) return;
        act = &g_acct[g_acct_n++];
        memset(act, 0, sizeof *act);
        act->id = g_acct_next++;
        g_acct_active = act->id;
    }
    xt_acct_from_cfg(act);
    xt_accts_save();
}

/* What was read for one account is not shown for another. The lists on disk
 * are kept: they are filed by their address, which holds the account. */
static void xt_forget_memory(void)
{
    for (int s = 0; s < 2; ++s) {
        free(g_xt_new[s]); g_xt_new[s] = NULL; g_xt_new_at[s] = 0;
        free(g_xt_ix[s]);  g_xt_ix[s] = NULL;  g_xt_ix_n[s] = 0; g_xt_ix_at[s] = 0;
    }
    for (int i = 0; i < XT_SERIES_SLOTS; ++i) {
        free(g_xt_series[i].eps);
        memset(&g_xt_series[i], 0, sizeof g_xt_series[i]);
    }
    g_xt_play_url[0] = g_xt_play_ep[0] = g_xt_fetching[0] = 0;
    memset(g_xt_art, 0, sizeof g_xt_art);
    g_xt_art_next = 0;
    if (g_xt_ch) memset(g_xt_ch, 0, (size_t)XT_CH_SLOTS * sizeof *g_xt_ch);
    g_xt_ch_n = 0;
    g_xt_tz_known = 0;
    g_xt_tz_off = 0;
}

/* Makes account `id` the one in use. 0 done, -1 no such account. */
static int xt_switch(int id)
{
    if (id == g_acct_active && g_cfg.host[0]) return 0;
    const xt_acct_t *a = xt_acct_by_id(id);
    if (!a) return -1;
    xt_forget_memory();
    memset(&g_cfg, 0, sizeof g_cfg);
    snprintf(g_cfg.host, sizeof g_cfg.host, "%s", a->host);
    g_cfg.port = a->port;
    snprintf(g_cfg.username, sizeof g_cfg.username, "%s", a->user);
    snprintf(g_cfg.password, sizeof g_cfg.password, "%s", a->pass);
    g_cfg.use_https = a->https != 0;
    snprintf(g_cfg.stream_format, sizeof g_cfg.stream_format, "%s", a->fmt[0] ? a->fmt : "m3u8");
    g_acct_active = id;
    xtream_save_config();
    char gu[EVO_EPG_MAX_URL];
    build_xmltv_url(gu, sizeof gu);
    evo_epg_set_provider_source("xtream", gu);
    PROV_LOG("xtream: now using account %d", id);
    return 0;
}

int xtream_account_count(void)
{
    return g_acct_n;
}

/* Settings / Options: forget account `id`. The next one takes over. */
int xtream_remove_account(int id)
{
    int at = -1;
    for (int i = 0; i < g_acct_n; ++i)
        if (g_acct[i].id == id) at = i;
    if (at < 0) return -1;
    memmove(&g_acct[at], &g_acct[at + 1], (size_t)(g_acct_n - at - 1) * sizeof g_acct[0]);
    g_acct_n--;
    if (id == g_acct_active) {
        g_acct_active = -1;
        xt_forget_memory();
        if (g_acct_n > 0) {
            memset(&g_cfg, 0, sizeof g_cfg);     /* so xt_switch does not keep it */
            xt_switch(g_acct[0].id);
        } else {
            memset(&g_cfg, 0, sizeof g_cfg);
            xtream_save_config();
        }
    }
    xt_accts_save();
    return 0;
}

/* "<n>|rest": the account number, and the id without it. -1 when there is
 * no number (account 0's ids, and every id from before accounts). */
static int xt_split_acct(const char *id, const char **rest)
{
    *rest = id;
    if (!id) return -1;
    const char *p = id;
    while (*p >= '0' && *p <= '9') p++;
    if (p == id || *p != '|' || p - id > 4) return -1;
    *rest = p + 1;
    return atoi(id);
}

/* The account an id belongs to; switched to when it is not in use.
 * 0 ready, -1 the account is gone. */
static int xt_use_acct_of(const char *id, const char **rest)
{
    const int n = xt_split_acct(id, rest);
    if (g_acct_n == 0) return 0;              /* not set up: the inner calls say so */
    return xt_switch(n < 0 ? 0 : n);
}

static void xt_add_prefix(char *id, size_t sz, int acct)
{
    if (acct <= 0 || !id[0] || strncmp(id, "acct:", 5) == 0) return;
    const char *rest;
    if (xt_split_acct(id, &rest) >= 0) return;
    char tmp[EVO_PROVIDER_MAX_ITEM_ID + 8];
    snprintf(tmp, sizeof tmp, "%d|%s", acct, id);
    snprintf(id, sz, "%s", tmp);
}

typedef struct {
    evo_provider_items_cb cb;
    void *ud;
    int   acct;
    int   fired, returned;
} xt_wrap_t;

static void xt_wrap_items(int ok, const evo_provider_item_t *items, int count, int more, void *ud)
{
    xt_wrap_t *w = (xt_wrap_t *)ud;
    evo_provider_item_t *copy = NULL;
    if (items && count > 0 && w->acct > 0) {
        copy = (evo_provider_item_t *)malloc((size_t)count * sizeof *copy);
        if (copy) {
            memcpy(copy, items, (size_t)count * sizeof *copy);
            for (int i = 0; i < count; ++i) {
                xt_add_prefix(copy[i].id, sizeof copy[i].id, w->acct);
                xt_add_prefix(copy[i].parent_id, sizeof copy[i].parent_id, w->acct);
            }
        }
    }
    if (w->cb) w->cb(ok, copy ? copy : items, count, more, w->ud);
    free(copy);
    w->fired = 1;
    if (w->returned) free(w);
}

static xt_wrap_t *xt_wrap_new(evo_provider_items_cb cb, void *ud)
{
    xt_wrap_t *w = (xt_wrap_t *)calloc(1, sizeof *w);
    if (!w) return NULL;
    w->cb = cb;
    w->ud = ud;
    w->acct = g_acct_active;
    return w;
}

/* After the inner call returned: free the wrapper once nobody will call it. */
static int xt_wrap_done(xt_wrap_t *w, int rc)
{
    w->returned = 1;
    if (w->fired || rc != 0) free(w);
    return rc;
}

static int xtream_list_catalog(const char *parent_id, int page, evo_provider_items_cb cb, void *ud)
{
    /* Root with more than one account: one folder each. */
    if ((!parent_id || !*parent_id) && g_acct_n > 1) {
        evo_provider_item_t items[XT_MAX_ACCTS];
        memset(items, 0, sizeof items);
        for (int i = 0; i < g_acct_n; ++i) {
            const xt_acct_t *a = &g_acct[i];
            evo_provider_item_clear(&items[i]);
            snprintf(items[i].id, sizeof items[i].id, "acct:%d", a->id);
            snprintf(items[i].title, sizeof items[i].title, "%s", a->host);
            snprintf(items[i].subtitle, sizeof items[i].subtitle, "%s%s", a->user,
                     a->id == g_acct_active ? "  -  in use" : "");
            items[i].kind = EVO_MEDIA_FOLDER;
            items[i].is_folder = 1;
        }
        if (cb) cb(1, items, g_acct_n, 0, ud);
        return 0;
    }
    const char *rest = parent_id;
    if (parent_id && strncmp(parent_id, "acct:", 5) == 0) {
        if (xt_switch(atoi(parent_id + 5)) != 0) { if (cb) cb(0, NULL, 0, 0, ud); return -1; }
        rest = "";
    } else if (!parent_id || !*parent_id) {
        if (g_acct_n == 1) xt_switch(g_acct[0].id);
    } else if (xt_use_acct_of(parent_id, &rest) != 0) {
        if (cb) cb(0, NULL, 0, 0, ud);
        return -1;
    }
    if (!cb) return xt_list_inner(rest, page, NULL, NULL);
    xt_wrap_t *w = xt_wrap_new(cb, ud);
    if (!w) return xt_list_inner(rest, page, cb, ud);
    return xt_wrap_done(w, xt_list_inner(rest, page, xt_wrap_items, w));
}

static int xtream_search(const char *query, int page, evo_provider_items_cb cb, void *ud)
{
    if (g_acct_n > 0 && !xt_acct_by_id(g_acct_active)) xt_switch(g_acct[0].id);
    if (!cb) return xt_search_inner(query, page, NULL, NULL);
    xt_wrap_t *w = xt_wrap_new(cb, ud);
    if (!w) return xt_search_inner(query, page, cb, ud);
    return xt_wrap_done(w, xt_search_inner(query, page, xt_wrap_items, w));
}

static int xtream_resolve(const char *item_id, evo_provider_resolve_cb cb, void *ud)
{
    const char *rest = item_id;
    if (xt_use_acct_of(item_id, &rest) != 0) return -1;
    return xt_resolve_inner(rest, cb, ud);
}

/* ------------------------------------------------------------------------- */
/* Lifecycle & vtable                                                        */
/* ------------------------------------------------------------------------- */

static int xtream_provider_init(void)
{
    return xtream_init();
}

static void xtream_provider_shutdown(void)
{
}

static int xtream_provider_is_configured(void)
{
    return (g_cfg.host[0] && g_cfg.username[0] && g_cfg.password[0]) ? 1 : 0;
}

static int xtream_provider_auth(evo_provider_auth_cb cb, void *ud)
{
    return xtream_connect_async((xtream_auth_cb)cb, ud);
}

static const char *xtream_provider_get_source(void)
{
    if (!g_cfg.host[0]) return "";
    const char *scheme = g_cfg.use_https ? "https" : "http";
    int default_port = g_cfg.use_https ? 443 : 80;
    if (g_cfg.port > 0 && g_cfg.port != default_port) {
        snprintf(g_source, sizeof g_source, "%s://%s:%d/get.php?username=%s&password=%s",
                 scheme, g_cfg.host, g_cfg.port, g_cfg.username, g_cfg.password);
    } else {
        snprintf(g_source, sizeof g_source, "%s://%s/get.php?username=%s&password=%s",
                 scheme, g_cfg.host, g_cfg.username, g_cfg.password);
    }
    return g_source;
}

/*
 * An M3U exported from an Xtream panel carries the account in every stream
 * link: http://host:port/live/USER/PASS/123.ts, the short form
 * http://host/USER/PASS/123, or a get.php?username=..&password=.. link.
 *
 * "Every" is the test: a public list can hold the odd Xtream-shaped link from
 * some stray server (iptv.m3u had one in 738), and that is not the user's
 * account. So the account has to be behind at least half of the file's links.
 * Fills `out` with the get.php form set_source understands; -1 = a plain
 * playlist, which belongs to the IPTV provider, not this one.
 */
static int xtream_link_account(const char *line, char *out, size_t out_sz)
{
    int https = strncmp(line, "https://", 8) == 0;
    if (!https && strncmp(line, "http://", 7) != 0) return -1;

    const char *u = strstr(line, "username=");
    const char *pw = strstr(line, "password=");
    if (u && pw) {
        /* Keep only scheme://host/get.php?username=..&password=.. */
        const char *hp = line + (https ? 8 : 7);
        const char *sl = strchr(hp, '/');
        size_t hl = sl ? (size_t)(sl - hp) : strlen(hp);
        u += 9; pw += 9;
        size_t ul = strcspn(u, "&"), pl = strcspn(pw, "&");
        snprintf(out, out_sz, "%s://%.*s/get.php?username=%.*s&password=%.*s",
                 https ? "https" : "http", (int)hl, hp, (int)ul, u, (int)pl, pw);
        return 0;
    }

    const char *hp = line + (https ? 8 : 7);
    const char *path_start = strchr(hp, '/');
    if (!path_start) return -1;
    size_t hl = (size_t)(path_start - hp);
    if (hl == 0 || hl >= 160) return -1;

    char segbuf[512];
    snprintf(segbuf, sizeof segbuf, "%s", path_start + 1);
    char *q = strchr(segbuf, '?');
    if (q) *q = 0;
    char *seg[6];
    int ns = 0;
    for (char *p = segbuf; *p && ns < 6; ) {
        seg[ns++] = p;
        char *sl = strchr(p, '/');
        if (!sl) break;
        *sl = 0;
        p = sl + 1;
    }

    const char *user = NULL, *pass = NULL;
    if (ns >= 4 && (strcmp(seg[0], "live") == 0 || strcmp(seg[0], "movie") == 0 ||
                    strcmp(seg[0], "series") == 0)) {
        user = seg[1]; pass = seg[2];
    } else if (ns == 3 && seg[2][0] >= '0' && seg[2][0] <= '9') {
        user = seg[0]; pass = seg[1];
    }
    if (!user || !pass || !*user || !*pass) return -1;
    snprintf(out, out_sz, "%s://%.*s/get.php?username=%s&password=%s",
             https ? "https" : "http", (int)hl, hp, user, pass);
    return 0;
}

static int xtream_url_from_m3u(const char *path, char *out, size_t out_sz)
{
    FILE *f = fopen(path, "rb");
    if (!f) return -1;

    enum { MAXA = 8 };
    static char acct[MAXA][512];
    int votes[MAXA] = {0};
    int na = 0, links = 0, lines = 0;
    char line[1024], cand[512];

    while (lines < 20000 && fgets(line, sizeof line, f)) {
        lines++;
        size_t n = strlen(line);
        while (n && (line[n - 1] == '\n' || line[n - 1] == '\r' || line[n - 1] == ' ')) line[--n] = 0;
        if (strncmp(line, "http", 4) != 0) continue;
        links++;
        if (xtream_link_account(line, cand, sizeof cand) != 0) continue;
        int i = 0;
        while (i < na && strcmp(acct[i], cand) != 0) i++;
        if (i == na) {
            if (na == MAXA) continue;
            snprintf(acct[na++], sizeof acct[0], "%s", cand);
        }
        votes[i]++;
    }
    fclose(f);

    int best = -1;
    for (int i = 0; i < na; ++i)
        if (best < 0 || votes[i] > votes[best]) best = i;
    if (best < 0 || votes[best] * 2 < links) {
        PROV_LOG("xtream: playlist has %d links, best account behind %d - not an Xtream export",
                 links, best < 0 ? 0 : votes[best]);
        return -1;
    }
    snprintf(out, out_sz, "%s", acct[best]);
    return 0;
}

static int xtream_provider_set_source(const char *value);

static void xtream_provider_sign_out(void)
{
    xtream_provider_set_source("");      /* "" clears the account and saves */
}

static int xtream_provider_set_source(const char *value)
{
    if (!value || !*value) {
        /* Signing out: this account goes, the next one (if any) takes over. */
        if (xt_acct_by_id(g_acct_active)) return xtream_remove_account(g_acct_active);
        memset(&g_cfg, 0, sizeof g_cfg);
        xtream_save_config();
        return 0;
    }

    /* A playlist file from the USB picker: Xtream needs the account inside it. */
    char from_file[1024];
    if (value[0] == '/') {
        if (xtream_url_from_m3u(value, from_file, sizeof from_file) != 0) {
            PROV_LOG("xtream: %s has no Xtream account in its links", value);
            return -1;
        }
        PROV_LOG("xtream: account read from %s", value);
        value = from_file;
    }

    char host[128] = {0};
    int port = 80;
    char user[64] = {0};
    char pass[64] = {0};
    bool https = false;

    /* Check format: http(s)://... */
    if (strncmp(value, "http://", 7) == 0 || strncmp(value, "https://", 8) == 0) {
        https = (strncmp(value, "https://", 8) == 0);
        const char *p = value + (https ? 8 : 7);
        port = https ? 443 : 80;

        /* Check for get.php?username=...&password=... */
        const char *u_param = strstr(p, "username=");
        const char *p_param = strstr(p, "password=");
        if (u_param && p_param) {
            u_param += 9;
            const char *u_end = strchr(u_param, '&');
            if (!u_end) u_end = u_param + strlen(u_param);
            size_t u_len = (size_t)(u_end - u_param);
            if (u_len >= sizeof user) u_len = sizeof user - 1;
            memcpy(user, u_param, u_len);

            p_param += 9;
            const char *p_end = strchr(p_param, '&');
            if (!p_end) p_end = p_param + strlen(p_param);
            size_t p_len = (size_t)(p_end - p_param);
            if (p_len >= sizeof pass) p_len = sizeof pass - 1;
            memcpy(pass, p_param, p_len);
        }

        /* Check for user:pass@host */
        const char *at = strchr(p, '@');
        const char *slash = strchr(p, '/');
        if (at && (!slash || at < slash)) {
            const char *colon = strchr(p, ':');
            if (colon && colon < at) {
                size_t ul = (size_t)(colon - p);
                if (ul >= sizeof user) ul = sizeof user - 1;
                memcpy(user, p, ul);

                size_t pl = (size_t)(at - (colon + 1));
                if (pl >= sizeof pass) pl = sizeof pass - 1;
                memcpy(pass, colon + 1, pl);
            }
            p = at + 1;
        }

        /* Extract host[:port] */
        const char *h_end = strchr(p, '/');
        if (!h_end) h_end = strchr(p, '?');
        if (!h_end) h_end = p + strlen(p);

        const char *colon = strchr(p, ':');
        if (colon && colon < h_end) {
            size_t hl = (size_t)(colon - p);
            if (hl >= sizeof host) hl = sizeof host - 1;
            memcpy(host, p, hl);
            port = atoi(colon + 1);
        } else {
            size_t hl = (size_t)(h_end - p);
            if (hl >= sizeof host) hl = sizeof host - 1;
            memcpy(host, p, hl);
        }
    } else {
        /* Format: host:port:user:pass */
        const char *c1 = strchr(value, ':');
        if (c1) {
            size_t hl = (size_t)(c1 - value);
            if (hl >= sizeof host) hl = sizeof host - 1;
            memcpy(host, value, hl);

            const char *c2 = strchr(c1 + 1, ':');
            if (c2) {
                port = atoi(c1 + 1);
                const char *c3 = strchr(c2 + 1, ':');
                if (c3) {
                    size_t ul = (size_t)(c3 - (c2 + 1));
                    if (ul >= sizeof user) ul = sizeof user - 1;
                    memcpy(user, c2 + 1, ul);
                    snprintf(pass, sizeof pass, "%s", c3 + 1);
                }
            }
        }
    }

    if (!host[0]) return -1;
    /* The same panel and user: that account, updated. Otherwise one more. */
    int id = -1;
    for (int i = 0; i < g_acct_n; ++i)
        if (strcasecmp(g_acct[i].host, host) == 0 && strcmp(g_acct[i].user, user) == 0) id = g_acct[i].id;
    if (id >= 0) {
        xt_switch(id);
    } else if (g_acct_n >= XT_MAX_ACCTS) {
        PROV_LOG("xtream: already %d accounts", g_acct_n);
        return -1;
    } else if (g_cfg.host[0] || g_acct_n > 0) {
        xt_forget_memory();
        char fmt[16];
        snprintf(fmt, sizeof fmt, "%s", g_cfg.stream_format[0] ? g_cfg.stream_format : "m3u8");
        memset(&g_cfg, 0, sizeof g_cfg);
        snprintf(g_cfg.stream_format, sizeof g_cfg.stream_format, "%s", fmt);
        g_acct_active = -1;                  /* xt_accts_sync adds it */
    }
    snprintf(g_cfg.host, sizeof g_cfg.host, "%s", host);
    g_cfg.port = port > 0 ? port : (https ? 443 : 80);
    if (user[0]) snprintf(g_cfg.username, sizeof g_cfg.username, "%s", user);
    if (pass[0]) snprintf(g_cfg.password, sizeof g_cfg.password, "%s", pass);
    g_cfg.use_https = https;
    if (!g_cfg.stream_format[0]) snprintf(g_cfg.stream_format, sizeof g_cfg.stream_format, "m3u8");
    g_cfg.status[0] = g_cfg.exp_date[0] = 0;
    const int rc = xtream_save_config();
    char gu[EVO_EPG_MAX_URL];
    build_xmltv_url(gu, sizeof gu);
    evo_epg_set_provider_source("xtream", gu);
    return rc;
}

static const char *xtream_provider_ui_bundle_url(void)
{
    return NULL;
}

const evo_provider_t evo_provider_xtream = {
    .id              = "xtream",
    .name            = "Xtream Codes",
    .icon            = "icon_tv.png",
    .caps            = EVO_PROVIDER_CAP_CATALOG | EVO_PROVIDER_CAP_SEARCH |
                       EVO_PROVIDER_CAP_RESOLVE |
                       EVO_PROVIDER_CAP_AUTH    | EVO_PROVIDER_CAP_UI |
                       EVO_PROVIDER_CAP_CONFIG  | EVO_PROVIDER_CAP_LIVE,
    .api_version     = EVO_PROVIDER_API_VERSION,
    .init            = xtream_provider_init,
    .shutdown        = xtream_provider_shutdown,
    .is_configured   = xtream_provider_is_configured,
    .auth            = xtream_provider_auth,
    .list_catalog    = xtream_list_catalog,
    .search          = xtream_search,
    .resolve         = xtream_resolve,
    .report_progress = NULL,
    .ui_bundle_url   = xtream_provider_ui_bundle_url,
    .get_source      = xtream_provider_get_source,
    .set_source      = xtream_provider_set_source,
    .web_ui_url      = NULL,
    .web_ui_path     = NULL,
    .web_ui_hook     = NULL,
    .is_signed_in    = xtream_provider_is_configured,
    .sign_out        = xtream_provider_sign_out,
};
