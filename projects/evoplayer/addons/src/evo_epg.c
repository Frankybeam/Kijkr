/*
 * evo_epg.c - the shared XMLTV guide. See evo_epg.h for what it is for.
 *
 * A guide is parsed once into three flat arrays - channels, programmes and one
 * string pool - plus two hash tables (guide id -> channel, loose name ->
 * channel). Only programmes from two days ago to two days ahead are kept (the
 * past for catch-up in the TV guide grid), at most EPG_MAX_PROGS of them, and
 * a description only while the string pool is under EPG_DESC_POOL: a big
 * international guide runs to millions of entries.
 *
 * The parser walks the bytes the same way provider_iptv.c's does: <channel>
 * for ids and display names, then <programme> for start, stop, channel and the
 * first <title>. Anything malformed is skipped. Every search is bounded by the
 * buffer end, so a body without a terminating NUL is safe.
 */
#include "evo_epg.h"
#include "evo_data_path.h"
#include "evo_net.h"
#include "evo_provider_log.h"

#include <ctype.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <zlib.h>

/* The provider list screen, when it is linked in: re-read the level so the
 * rows pick up a guide that arrived after they were built. */
__attribute__((weak)) void evo_rmlui_provider_reload(void);

#define EPG_SLOTS          5             /* 0 = the user's guide */
#define EPG_MAX_AGE_S      (12 * 3600)   /* download again after this */
#define EPG_RETRY_S        (10 * 60)     /* after a failure */
#define EPG_KEEP_BEFORE_S  (48 * 3600)
#define EPG_KEEP_AFTER_S   (48 * 3600)
#define EPG_MAX_INFLATED   ((size_t)256 * 1024 * 1024)
#define EPG_MAX_FILE       ((size_t)256 * 1024 * 1024)
#define EPG_MAX_PROGS      600000
#define EPG_DESC_POOL      ((size_t)48 * 1024 * 1024)
#define EPG_DESC_MAX       200

/* ------------------------------------------------------------------------- */
/* Small string helpers (bounded; nothing the console's libc might lack)     */
/* ------------------------------------------------------------------------- */

static const char *find(const char *p, const char *end, const char *needle)
{
    const size_t nl = strlen(needle);
    if (!nl) return p;
    for (; p && p + nl <= end; ++p) {
        p = memchr(p, needle[0], (size_t)(end - p));
        if (!p || p + nl > end) return NULL;
        if (memcmp(p, needle, nl) == 0) return p;
    }
    return NULL;
}

/* Value of attr="..." (or attr='...') inside one element's opening tag. */
static int tag_attr(const char *el, const char *el_end, const char *attr,
                    char *out, size_t out_sz)
{
    const size_t al = strlen(attr);
    for (const char *p = el; p + al < el_end; ++p) {
        if (memcmp(p, attr, al) != 0) continue;
        if (p != el && !isspace((unsigned char)p[-1])) continue;
        const char *q = p + al;
        while (q < el_end && isspace((unsigned char)*q)) q++;
        if (q >= el_end || *q != '=') continue;
        q++;
        while (q < el_end && isspace((unsigned char)*q)) q++;
        if (q >= el_end || (*q != '"' && *q != '\'')) continue;
        const char quote = *q++;
        const char *e = memchr(q, quote, (size_t)(el_end - q));
        if (!e) return -1;
        size_t n = (size_t)(e - q);
        if (n >= out_sz) n = out_sz - 1;
        memcpy(out, q, n);
        out[n] = '\0';
        return 0;
    }
    return -1;
}

static void decode_entities(char *s)
{
    char *r = s, *w = s;
    while (*r) {
        if (*r == '&') {
            if (!strncmp(r, "&amp;", 5))       { *w++ = '&';  r += 5; continue; }
            if (!strncmp(r, "&quot;", 6))      { *w++ = '"';  r += 6; continue; }
            if (!strncmp(r, "&apos;", 6))      { *w++ = '\''; r += 6; continue; }
            if (!strncmp(r, "&#39;", 5))       { *w++ = '\''; r += 5; continue; }
            if (!strncmp(r, "&lt;", 4))        { *w++ = '<';  r += 4; continue; }
            if (!strncmp(r, "&gt;", 4))        { *w++ = '>';  r += 4; continue; }
        }
        *w++ = *r++;
    }
    *w = '\0';
}

static void trim(char *s)
{
    size_t n = strlen(s);
    while (n && isspace((unsigned char)s[n - 1])) s[--n] = '\0';
    size_t lead = 0;
    while (s[lead] && isspace((unsigned char)s[lead])) lead++;
    if (lead) memmove(s, s + lead, n - lead + 1);
}

/* Text between <tag ...> and </tag>, starting the search at p. */
static int element_text(const char *p, const char *end, const char *open,
                        const char *close, char *out, size_t osz)
{
    const char *o = find(p, end, open);
    if (!o) return -1;
    const char *gt = memchr(o, '>', (size_t)(end - o));
    if (!gt) return -1;
    const char *c = find(gt + 1, end, close);
    if (!c) return -1;
    size_t n = (size_t)(c - gt - 1);
    if (n >= osz) n = osz - 1;
    memcpy(out, gt + 1, n);
    out[n] = '\0';
    /* <![CDATA[...]]> */
    if (!strncmp(out, "<![CDATA[", 9)) {
        memmove(out, out + 9, strlen(out + 9) + 1);
        char *cd = strstr(out, "]]>");
        if (cd) *cd = '\0';
    } else {
        decode_entities(out);
    }
    trim(out);
    return 0;
}

/*
 * The loose name a channel is matched by when its guide id is unknown:
 * lower-case letters and digits only, (...) and [...] dropped, a country
 * prefix ("NL:", "UK |") and a trailing quality tag (HD, FHD, UHD, SD, 4K)
 * cut. Same idea as provider_iptv.c's norm_name, plus the prefix, which
 * Xtream and Stalker channel names nearly always carry.
 */
static void norm_name(const char *s, char *out, size_t osz)
{
    /* "NL: ", "NL | ", "NL - ", "[NL] " */
    const char *p = s;
    if (isalpha((unsigned char)p[0]) && isalpha((unsigned char)p[1])) {
        const char *q = p + 2;
        if (isalpha((unsigned char)*q)) q++;              /* three letters */
        while (*q == ' ') q++;
        if (*q == ':' || *q == '|' || *q == '-') {
            q++;
            while (*q == ' ') q++;
            if (*q) p = q;
        }
    }
    size_t n = 0;
    int depth = 0;
    for (; *p && n + 1 < osz; ++p) {
        unsigned char c = (unsigned char)*p;
        if (c == '(' || c == '[') { depth++; continue; }
        if (c == ')' || c == ']') { if (depth) depth--; continue; }
        if (depth) continue;
        if (isalnum(c)) out[n++] = (char)tolower(c);
    }
    out[n] = 0;
    static const char *const tags[] = { "fhd", "uhd", "hd", "sd", "4k", "hevc", NULL };
    for (int again = 1; again; ) {
        again = 0;
        for (int t = 0; tags[t]; ++t) {
            size_t tl = strlen(tags[t]);
            if (n > tl + 2 && strcmp(out + n - tl, tags[t]) == 0) {
                n -= tl;
                out[n] = 0;
                again = 1;
                break;
            }
        }
    }
}

/* "20261012143000 +0200" -> Unix time. Days from the civil epoch (Howard
 * Hinnant's algorithm): timegm() is not portable here, and mktime() would apply
 * the console's zone to a time that carries its own offset. */
static int64_t xmltv_time(const char *s)
{
    int y, mo, d, h, mi, se = 0;
    if (sscanf(s, "%4d%2d%2d%2d%2d", &y, &mo, &d, &h, &mi) != 5) return -1;
    if (isdigit((unsigned char)s[12]) && isdigit((unsigned char)s[13]))
        se = (s[12] - '0') * 10 + (s[13] - '0');
    int yy = y - (mo <= 2);
    int era = (yy >= 0 ? yy : yy - 399) / 400;
    unsigned yoe = (unsigned)(yy - era * 400);
    unsigned doy = (unsigned)((153 * (mo + (mo > 2 ? -3 : 9)) + 2) / 5 + d - 1);
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    int64_t days = (int64_t)era * 146097 + (int64_t)doe - 719468;
    int64_t t = days * 86400 + h * 3600 + mi * 60 + se;
    const char *sp = strchr(s, ' ');
    if (sp) {
        while (*sp == ' ') sp++;
        if ((*sp == '+' || *sp == '-') && strlen(sp) >= 5) {
            int sign = (*sp == '-') ? -1 : 1;
            int oh = (sp[1] - '0') * 10 + (sp[2] - '0');
            int om = (sp[3] - '0') * 10 + (sp[4] - '0');
            t -= sign * (oh * 3600 + om * 60);
        }
    }
    return t;
}

static uint32_t hash_str(const char *s)
{
    uint32_t h = 2166136261u;
    while (*s) { h ^= (unsigned char)*s++; h *= 16777619u; }
    return h;
}

/* ------------------------------------------------------------------------- */
/* gzip                                                                      */
/* ------------------------------------------------------------------------- */

char *evo_epg_gunzip(const char *buf, size_t len, size_t *out_len)
{
    if (!buf || len < 18 || (unsigned char)buf[0] != 0x1f || (unsigned char)buf[1] != 0x8b)
        return NULL;
    size_t cap = len * 8 + 4096, n = 0;
    if (cap > EPG_MAX_INFLATED) cap = EPG_MAX_INFLATED;
    char *out = (char *)malloc(cap + 1);
    if (!out) return NULL;

    z_stream zs;
    memset(&zs, 0, sizeof zs);
    if (inflateInit2(&zs, 16 + MAX_WBITS) != Z_OK) { free(out); return NULL; }
    zs.next_in = (Bytef *)buf;
    zs.avail_in = (uInt)len;
    int rc = Z_OK;
    for (;;) {
        if (n == cap) {
            if (cap >= EPG_MAX_INFLATED) { rc = Z_MEM_ERROR; break; }
            size_t ncap = cap * 2 > EPG_MAX_INFLATED ? EPG_MAX_INFLATED : cap * 2;
            char *g = (char *)realloc(out, ncap + 1);
            if (!g) { rc = Z_MEM_ERROR; break; }
            out = g;
            cap = ncap;
        }
        zs.next_out = (Bytef *)(out + n);
        zs.avail_out = (uInt)(cap - n);
        rc = inflate(&zs, Z_NO_FLUSH);
        n = cap - zs.avail_out;
        if (rc == Z_STREAM_END) {
            /* Concatenated gzip members: carry on with the next one. */
            if (zs.avail_in > 0 && inflateReset(&zs) == Z_OK) continue;
            break;
        }
        if (rc != Z_OK) break;
    }
    inflateEnd(&zs);
    if (rc != Z_STREAM_END || n == 0) {
        free(out);
        return NULL;
    }
    out[n] = '\0';
    if (out_len) *out_len = n;
    return out;
}

/* ------------------------------------------------------------------------- */
/* A parsed guide                                                            */
/* ------------------------------------------------------------------------- */

typedef struct { int64_t start, stop; uint32_t title, desc; int32_t ch; } prog_t;
typedef struct { uint32_t id; int32_t first, n; } chan_t;

typedef struct {
    char    *pool;  size_t pool_n, pool_cap;
    chan_t  *ch;    int    nch,    ch_cap;
    prog_t  *pr;    int    npr,    pr_cap;
    /* open addressing, value = channel index + 1 (0 = empty) */
    int32_t *id_ht;   uint32_t id_mask;
    uint32_t *nm_key; int32_t *nm_val; uint32_t nm_mask; int nm_n;
} guide_t;

static uint32_t pool_add(guide_t *g, const char *s)
{
    size_t l = strlen(s) + 1;
    if (g->pool_n + l > g->pool_cap) {
        size_t nc = g->pool_cap ? g->pool_cap * 2 : 65536;
        while (nc < g->pool_n + l) nc *= 2;
        char *p = (char *)realloc(g->pool, nc);
        if (!p) return 0;
        g->pool = p;
        g->pool_cap = nc;
    }
    uint32_t off = (uint32_t)g->pool_n;
    memcpy(g->pool + off, s, l);
    g->pool_n += l;
    return off;
}

static void guide_free(guide_t *g)
{
    if (!g) return;
    free(g->pool); free(g->ch); free(g->pr); free(g->id_ht); free(g->nm_key); free(g->nm_val);
    free(g);
}

static int ht_grow_ids(guide_t *g)
{
    uint32_t size = 1024;
    while (size < (uint32_t)g->nch * 2 + 16) size <<= 1;
    if (g->id_ht && size - 1 == g->id_mask) return 0;
    int32_t *t = (int32_t *)calloc(size, sizeof *t);
    if (!t) return -1;
    for (int i = 0; i < g->nch; ++i) {
        uint32_t h = hash_str(g->pool + g->ch[i].id) & (size - 1);
        while (t[h]) h = (h + 1) & (size - 1);
        t[h] = i + 1;
    }
    free(g->id_ht);
    g->id_ht = t;
    g->id_mask = size - 1;
    return 0;
}

static int find_id(const guide_t *g, const char *id)
{
    if (!g->id_ht || !id || !id[0]) return -1;
    uint32_t h = hash_str(id) & g->id_mask;
    while (g->id_ht[h]) {
        int i = g->id_ht[h] - 1;
        if (strcmp(g->pool + g->ch[i].id, id) == 0) return i;
        h = (h + 1) & g->id_mask;
    }
    return -1;
}

static int add_channel(guide_t *g, const char *id)
{
    if (g->nch == g->ch_cap) {
        int nc = g->ch_cap ? g->ch_cap * 2 : 1024;
        chan_t *c = (chan_t *)realloc(g->ch, (size_t)nc * sizeof *c);
        if (!c) return -1;
        g->ch = c;
        g->ch_cap = nc;
    }
    chan_t *c = &g->ch[g->nch];
    c->id = pool_add(g, id);
    c->first = c->n = 0;
    g->nch++;
    if ((uint32_t)g->nch * 2 + 16 > g->id_mask + 1 || !g->id_ht) {
        if (ht_grow_ids(g) != 0) return -1;
    } else {
        uint32_t h = hash_str(id) & g->id_mask;
        while (g->id_ht[h]) h = (h + 1) & g->id_mask;
        g->id_ht[h] = g->nch;
    }
    return g->nch - 1;
}

/* Loose names: the first channel to claim a name keeps it. Keys are the
 * name's hash only - two different names with the same 32-bit hash would
 * share a slot, a risk worth a channel's now/next, not a crash. */
static void name_put(guide_t *g, const char *norm, int ch)
{
    if (!norm[0]) return;
    if ((uint32_t)(g->nm_n + 1) * 2 > (g->nm_key ? g->nm_mask + 1 : 0)) {
        uint32_t size = g->nm_key ? (g->nm_mask + 1) * 2 : 2048;
        uint32_t *k = (uint32_t *)calloc(size, sizeof *k);
        int32_t *v = (int32_t *)calloc(size, sizeof *v);
        if (!k || !v) { free(k); free(v); return; }
        if (g->nm_key)
            for (uint32_t i = 0; i <= g->nm_mask; ++i) {
                if (!g->nm_val[i]) continue;
                uint32_t h = g->nm_key[i] & (size - 1);
                while (v[h]) h = (h + 1) & (size - 1);
                k[h] = g->nm_key[i];
                v[h] = g->nm_val[i];
            }
        free(g->nm_key); free(g->nm_val);
        g->nm_key = k; g->nm_val = v; g->nm_mask = size - 1;
    }
    const uint32_t key = hash_str(norm);
    uint32_t h = key & g->nm_mask;
    while (g->nm_val[h]) {
        if (g->nm_key[h] == key) return;
        h = (h + 1) & g->nm_mask;
    }
    g->nm_key[h] = key;
    g->nm_val[h] = ch + 1;
    g->nm_n++;
}

static int find_name(const guide_t *g, const char *raw)
{
    if (!g->nm_key || !raw || !raw[0]) return -1;
    char norm[128];
    norm_name(raw, norm, sizeof norm);
    if (!norm[0]) return -1;
    const uint32_t key = hash_str(norm);
    uint32_t h = key & g->nm_mask;
    while (g->nm_val[h]) {
        if (g->nm_key[h] == key) return g->nm_val[h] - 1;
        h = (h + 1) & g->nm_mask;
    }
    return -1;
}

static int prog_cmp(const void *a, const void *b)
{
    const prog_t *x = (const prog_t *)a, *y = (const prog_t *)b;
    if (x->ch != y->ch) return x->ch < y->ch ? -1 : 1;
    if (x->start != y->start) return x->start < y->start ? -1 : 1;
    return 0;
}

static guide_t *parse_xmltv(const char *body, size_t len)
{
    guide_t *g = (guide_t *)calloc(1, sizeof *g);
    if (!g) return NULL;
    const char *end = body + len;
    pool_add(g, "");                                   /* offset 0 = "" */

    /* Pass 1: <channel id="..."><display-name>...</display-name></channel> */
    for (const char *p = find(body, end, "<channel"); p; ) {
        const char *gt = memchr(p, '>', (size_t)(end - p));
        if (!gt) break;
        const char *close = find(gt, end, "</channel>");
        const char *ce = close ? close : end;
        char id[128] = "";
        if (p[8] == ' ' || p[8] == '\t' || p[8] == '\n' || p[8] == '\r') {
            tag_attr(p, gt, "id", id, sizeof id);
            decode_entities(id);
            trim(id);
        }
        if (id[0] && find_id(g, id) < 0) {
            int ci = add_channel(g, id);
            if (ci < 0) break;
            char norm[128];
            norm_name(id, norm, sizeof norm);
            name_put(g, norm, ci);
            for (const char *d = find(gt, ce, "<display-name"); d && d < ce;
                 d = find(d + 13, ce, "<display-name")) {
                char dn[160];
                if (element_text(d, ce, "<display-name", "</display-name>", dn, sizeof dn) == 0) {
                    norm_name(dn, norm, sizeof norm);
                    name_put(g, norm, ci);
                }
            }
        }
        p = find(close ? close + 10 : gt + 1, end, "<channel");
    }

    /* Pass 2: <programme start stop channel><title>...</title></programme> */
    const int64_t now = (int64_t)time(NULL);
    const int clock_ok = now > 1700000000;             /* a console clock that was never set */
    for (const char *p = find(body, end, "<programme"); p; ) {
        const char *gt = memchr(p, '>', (size_t)(end - p));
        if (!gt) break;
        const char *close = find(gt, end, "</programme>");
        const char *pe = close ? close : end;
        char ch[128] = "", st[40] = "", sp[40] = "";
        tag_attr(p, gt, "channel", ch, sizeof ch);
        tag_attr(p, gt, "start", st, sizeof st);
        tag_attr(p, gt, "stop", sp, sizeof sp);
        decode_entities(ch);
        trim(ch);
        const int64_t t0 = st[0] ? xmltv_time(st) : -1;
        int64_t t1 = sp[0] ? xmltv_time(sp) : -1;
        if (ch[0] && t0 > 0) {
            if (t1 <= t0) t1 = t0 + 3600;              /* no stop: assume an hour */
            if (!clock_ok || (t1 > now - EPG_KEEP_BEFORE_S && t0 < now + EPG_KEEP_AFTER_S)) {
                char title[200];
                if (element_text(gt, pe, "<title", "</title>", title, sizeof title) == 0 && title[0]) {
                    int ci = find_id(g, ch);
                    if (ci < 0) {                      /* guides without a <channel> list */
                        ci = add_channel(g, ch);
                        if (ci >= 0) {
                            char norm[128];
                            norm_name(ch, norm, sizeof norm);
                            name_put(g, norm, ci);
                        }
                    }
                    if (ci >= 0 && g->npr < EPG_MAX_PROGS) {
                        if (g->npr == g->pr_cap) {
                            int nc = g->pr_cap ? g->pr_cap * 2 : 8192;
                            prog_t *np = (prog_t *)realloc(g->pr, (size_t)nc * sizeof *np);
                            if (!np) break;
                            g->pr = np;
                            g->pr_cap = nc;
                        }
                        prog_t *pr = &g->pr[g->npr++];
                        pr->start = t0;
                        pr->stop = t1;
                        pr->ch = ci;
                        pr->title = pool_add(g, title);
                        pr->desc = 0;
                        if (g->pool_n < EPG_DESC_POOL) {
                            char desc[EPG_DESC_MAX * 2];
                            if (element_text(gt, pe, "<desc", "</desc>", desc, sizeof desc) == 0 && desc[0]) {
                                if (strlen(desc) > EPG_DESC_MAX) {
                                    size_t cut = EPG_DESC_MAX;
                                    while (cut > 0 && ((unsigned char)desc[cut] & 0xC0) == 0x80) cut--;
                                    desc[cut] = 0;
                                    strcat(desc, "...");
                                }
                                pr->desc = pool_add(g, desc);
                            }
                        }
                    }
                }
            }
        }
        p = find(close ? close + 12 : gt + 1, end, "<programme");
    }

    if (g->npr > 1) qsort(g->pr, (size_t)g->npr, sizeof *g->pr, prog_cmp);
    for (int i = 0; i < g->npr; ) {
        int j = i;
        while (j < g->npr && g->pr[j].ch == g->pr[i].ch) j++;
        g->ch[g->pr[i].ch].first = i;
        g->ch[g->pr[i].ch].n = j - i;
        i = j;
    }
    if (g->nch == 0) { guide_free(g); return NULL; }
    return g;
}

/* now/next of one channel. 1 when anything was found. */
static int guide_now_next(const guide_t *g, int ci, char *now_t, size_t nsz,
                          char *next_t, size_t xsz)
{
    if (!g || ci < 0 || ci >= g->nch || g->ch[ci].n == 0) return 0;
    const int64_t now = (int64_t)time(NULL);
    const prog_t *pr = g->pr + g->ch[ci].first;
    const int n = g->ch[ci].n;
    for (int i = 0; i < n; ++i) {
        if (pr[i].stop <= now) continue;
        if (pr[i].start <= now) {
            snprintf(now_t, nsz, "%s", g->pool + pr[i].title);
            next_t[0] = 0;
            if (i + 1 < n) snprintf(next_t, xsz, "%s", g->pool + pr[i + 1].title);
        } else {                                       /* between programmes */
            now_t[0] = 0;
            snprintf(next_t, xsz, "%s", g->pool + pr[i].title);
        }
        return 1;
    }
    return 0;
}

static int guide_find(const guide_t *g, const char *epg_id, const char *name)
{
    if (!g) return -1;
    int ci = find_id(g, epg_id);
    if (ci < 0 && epg_id) ci = find_name(g, epg_id);
    if (ci < 0) ci = find_name(g, name);
    return ci;
}

/* ------------------------------------------------------------------------- */
/* Slots: where each guide comes from and its state                          */
/* ------------------------------------------------------------------------- */

typedef enum { ST_NONE, ST_LOADING, ST_READY, ST_FAILED, ST_EMPTY } slot_state_t;

typedef struct {
    char     key[16];
    char     url[EVO_EPG_MAX_URL];
    guide_t *g;
    slot_state_t state;
    time_t   at;              /* when the guide in memory was fetched */
    time_t   tried;           /* last attempt */
    unsigned req;             /* id of the request in flight */
    int      busy;
    int      fed;             /* the provider hands the guide over itself (evo_epg_feed) */
    unsigned fed_sig;         /* which body it was */
} slot_t;

static slot_t   g_slot[EPG_SLOTS];
static int      g_init;
static int      g_prefer_user;
static unsigned g_gen;
static unsigned g_req_seq;

static const char *conf_path(void) { return evo_data_path("epg.conf"); }

static void cache_paths(const slot_t *s, char *body, size_t bsz, char *meta, size_t msz)
{
    char leaf[64];
    snprintf(leaf, sizeof leaf, "epg_%s.cache", s->key);
    snprintf(body, bsz, "%s", evo_data_path(leaf));
    snprintf(leaf, sizeof leaf, "epg_%s.meta", s->key);
    snprintf(meta, msz, "%s", evo_data_path(leaf));
}

static void save_conf(void)
{
    FILE *f = fopen(conf_path(), "w");
    if (!f) return;
    fprintf(f, "url=%s\nprefer_user=%d\n", g_slot[0].url, g_prefer_user);
    fclose(f);
}

static void init(void)
{
    if (g_init) return;
    g_init = 1;
    snprintf(g_slot[0].key, sizeof g_slot[0].key, "user");
    FILE *f = fopen(conf_path(), "r");
    if (!f) return;
    char line[EVO_EPG_MAX_URL + 32];
    while (fgets(line, sizeof line, f)) {
        trim(line);
        if (!strncmp(line, "url=", 4))
            snprintf(g_slot[0].url, sizeof g_slot[0].url, "%s", line + 4);
        else if (!strncmp(line, "prefer_user=", 12))
            g_prefer_user = atoi(line + 12) ? 1 : 0;
    }
    fclose(f);
}

static char *read_file(const char *path, size_t *out_len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long sz = ftell(f);
    if (sz <= 0 || (size_t)sz > EPG_MAX_FILE) { fclose(f); return NULL; }
    rewind(f);
    char *buf = (char *)malloc((size_t)sz + 1);
    if (buf && fread(buf, 1, (size_t)sz, f) != (size_t)sz) { free(buf); buf = NULL; }
    fclose(f);
    if (buf) { buf[sz] = 0; *out_len = (size_t)sz; }
    return buf;
}

static void announce(void)
{
    g_gen++;
    if (evo_rmlui_provider_reload) evo_rmlui_provider_reload();
}

/*
 * Parsing runs on a worker thread: a national guide is tens of megabytes of
 * XML and takes seconds, which on the main thread froze the whole screen.
 * The worker builds a guide_t on its own and hands it over through g_done;
 * evo_epg_poll() (every frame) takes it on the main thread. Nothing else is
 * shared, and the worker does not log - PROV_LOG is a main-thread affair.
 */
typedef enum { JOB_NET, JOB_FILE, JOB_CACHE, JOB_FEED } job_kind_t;

typedef struct {
    int        slot;
    unsigned   req;
    job_kind_t kind;
    char      *body;          /* JOB_NET: the download, owned by the job */
    size_t     len;
    char       path[600];     /* JOB_FILE / JOB_CACHE: what to read */
    char       cache_body[512], cache_meta[512];
    char       url[EVO_EPG_MAX_URL];
    time_t     at;
} job_t;

static pthread_mutex_t g_mx = PTHREAD_MUTEX_INITIALIZER;
static struct {
    int        ready;
    unsigned   req;
    job_kind_t kind;
    guide_t   *g;
    time_t     at;
    size_t     len;
} g_done[EPG_SLOTS];

static void *job_run(void *arg)
{
    job_t *j = (job_t *)arg;
    char *buf = j->body;
    size_t len = j->len;
    if (!buf && j->path[0]) buf = read_file(j->path, &len);
    guide_t *g = NULL;
    if (buf) {
        size_t il = 0;
        char *plain = evo_epg_gunzip(buf, len, &il);
        g = plain ? parse_xmltv(plain, il) : parse_xmltv(buf, len);
        free(plain);
        if (g && j->kind == JOB_NET) {               /* keep it for the next start */
            FILE *f = fopen(j->cache_body, "wb");
            if (f) { fwrite(buf, 1, len, f); fclose(f); }
            f = fopen(j->cache_meta, "w");
            if (f) { fprintf(f, "%lld\n%s\n", (long long)j->at, j->url); fclose(f); }
        }
        free(buf);
    }
    pthread_mutex_lock(&g_mx);
    if (g_done[j->slot].ready) guide_free(g_done[j->slot].g);   /* superseded */
    g_done[j->slot].ready = 1;
    g_done[j->slot].req = j->req;
    g_done[j->slot].kind = j->kind;
    g_done[j->slot].g = g;
    g_done[j->slot].at = j->at;
    g_done[j->slot].len = len;
    pthread_mutex_unlock(&g_mx);
    free(j);
    return NULL;
}

static void job_start(slot_t *s, job_kind_t kind, char *body, size_t len, const char *path, time_t at)
{
    job_t *j = (job_t *)calloc(1, sizeof *j);
    if (!j) { free(body); s->busy = 0; if (!s->g) s->state = ST_FAILED; return; }
    j->slot = (int)(s - g_slot);
    j->req = s->req = ++g_req_seq;
    j->kind = kind;
    j->body = body;
    j->len = len;
    if (path) snprintf(j->path, sizeof j->path, "%s", path);
    cache_paths(s, j->cache_body, sizeof j->cache_body, j->cache_meta, sizeof j->cache_meta);
    snprintf(j->url, sizeof j->url, "%s", s->url);
    j->at = at;
    s->busy = 1;
    if (!s->g) s->state = ST_LOADING;

    pthread_t t;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_attr_setstacksize(&attr, 512 * 1024);
    const int rc = pthread_create(&t, &attr, job_run, j);
    pthread_attr_destroy(&attr);
    if (rc != 0) job_run(j);                         /* no thread: do it here */
}

static void on_guide(int success, int status, const char *body, size_t len, void *ud)
{
    const unsigned req = (unsigned)(uintptr_t)ud;
    slot_t *s = NULL;
    for (int i = 0; i < EPG_SLOTS; ++i)
        if (g_slot[i].busy && g_slot[i].req == req) s = &g_slot[i];
    if (!s) return;                                    /* the source changed meanwhile */
    if (!success || status != 200 || !body || !len) {
        s->busy = 0;
        PROV_LOG("epg: %s guide download failed (http=%d)", s->key, status);
        if (!s->g) s->state = ST_FAILED;
        return;
    }
    char *copy = (char *)malloc(len + 1);
    if (!copy) { s->busy = 0; if (!s->g) s->state = ST_FAILED; return; }
    memcpy(copy, body, len);
    copy[len] = 0;
    job_start(s, JOB_NET, copy, len, NULL, time(NULL));
}

void evo_epg_poll(void)
{
    int changed = 0;
    for (int i = 0; i < EPG_SLOTS; ++i) {
        pthread_mutex_lock(&g_mx);
        if (!g_done[i].ready) { pthread_mutex_unlock(&g_mx); continue; }
        const unsigned req = g_done[i].req;
        const job_kind_t kind = g_done[i].kind;
        guide_t *g = g_done[i].g;
        const time_t at = g_done[i].at;
        const size_t len = g_done[i].len;
        g_done[i].ready = 0;
        g_done[i].g = NULL;
        pthread_mutex_unlock(&g_mx);

        slot_t *s = &g_slot[i];
        if (!s->busy || s->req != req) { guide_free(g); continue; }   /* stale */
        s->busy = 0;
        if (g) {
            guide_free(s->g);
            s->g = g;
            s->state = ST_READY;
            s->at = at;
            PROV_LOG("epg: %s guide ready - %d channel(s), %d programme(s)%s", s->key,
                     g->nch, g->npr, kind == JOB_CACHE ? " (cache)" : "");
            changed = 1;
        } else if (kind == JOB_CACHE) {
            /* A bad cached copy: forget it and download at once. */
            char cb[512], cm[512];
            cache_paths(s, cb, sizeof cb, cm, sizeof cm);
            remove(cm);
            s->tried = 0;
            if (!s->g) s->state = ST_NONE;
        } else {
            PROV_LOG("epg: %s guide unusable (%zu bytes, %s)", s->key, len,
                     kind == JOB_FILE ? "file" : "download");
            if (!s->g) s->state = len ? ST_EMPTY : ST_FAILED;
        }
    }
    if (changed) announce();
}

/* Start loading the slot's guide when it has none, or when it is stale. */
static void ensure(slot_t *s)
{
    if (!s->url[0] || s->busy || s->fed) return;
    const time_t now = time(NULL);
    if (s->g && now - s->at < EPG_MAX_AGE_S) return;
    if (s->tried && now - s->tried < (s->g ? EPG_MAX_AGE_S : EPG_RETRY_S)) return;
    s->tried = now;

    /* A file on USB or in the data folder: read it again every time it is due. */
    if (s->url[0] == '/') {
        job_start(s, JOB_FILE, NULL, 0, s->url, now);
        return;
    }

    /* The cached download, when it is for this URL and fresh enough. */
    if (!s->g) {
        char cb[512], cm[512];
        cache_paths(s, cb, sizeof cb, cm, sizeof cm);
        FILE *f = fopen(cm, "r");
        if (f) {
            char ts[32] = "", url[EVO_EPG_MAX_URL] = "";
            const int ok = fgets(ts, sizeof ts, f) && fgets(url, sizeof url, f);
            fclose(f);
            if (ok) {
                trim(url);
                const time_t at = (time_t)atoll(ts);
                if (strcmp(url, s->url) == 0 && now - at < EPG_MAX_AGE_S) {
                    job_start(s, JOB_CACHE, NULL, 0, cb, at);
                    return;
                }
            }
        }
    }

    s->req = ++g_req_seq;
    s->busy = 1;
    if (!s->g) s->state = ST_LOADING;
    PROV_LOG("epg: downloading %s guide", s->key);
    if (evo_net_request_async("GET", s->url, NULL, NULL, 0, on_guide,
                              (void *)(uintptr_t)s->req) != 0) {
        s->busy = 0;
        if (!s->g) s->state = ST_FAILED;
    }
}

static void slot_reset(slot_t *s)
{
    guide_free(s->g);
    s->g = NULL;
    s->fed = 0;
    s->fed_sig = 0;
    s->state = ST_NONE;
    s->at = s->tried = 0;
    s->busy = 0;                                       /* a reply in flight is dropped by req */
}

static slot_t *slot_get(const char *key, int create)
{
    if (!key || !key[0]) return NULL;
    for (int i = 1; i < EPG_SLOTS; ++i)
        if (!strcmp(g_slot[i].key, key)) return &g_slot[i];
    if (!create) return NULL;
    for (int i = 1; i < EPG_SLOTS; ++i)
        if (!g_slot[i].key[0]) {
            snprintf(g_slot[i].key, sizeof g_slot[i].key, "%s", key);
            return &g_slot[i];
        }
    return NULL;
}

/* ------------------------------------------------------------------------- */
/* Public API                                                                */
/* ------------------------------------------------------------------------- */

const char *evo_epg_user_url(void)
{
    init();
    return g_slot[0].url;
}

int evo_epg_set_user_url(const char *url)
{
    init();
    char u[EVO_EPG_MAX_URL];
    snprintf(u, sizeof u, "%s", url ? url : "");
    trim(u);
    if (u[0] && u[0] != '/' && strncmp(u, "http://", 7) && strncmp(u, "https://", 8)) {
        /* "epg.example.com/guide.xml.gz" - the scheme is optional to type */
        if (strchr(u, ' ') || !strchr(u, '.')) return 0;
        char w[EVO_EPG_MAX_URL];
        snprintf(w, sizeof w, "http://%s", u);
        snprintf(u, sizeof u, "%s", w);
    }
    slot_reset(&g_slot[0]);
    snprintf(g_slot[0].url, sizeof g_slot[0].url, "%s", u);
    save_conf();
    ensure(&g_slot[0]);
    announce();
    return 1;
}

int evo_epg_prefer_user(void)
{
    init();
    return g_prefer_user;
}

void evo_epg_set_prefer_user(int on)
{
    init();
    g_prefer_user = on ? 1 : 0;
    save_conf();
    announce();
}

void evo_epg_refresh(void)
{
    init();
    for (int i = 0; i < EPG_SLOTS; ++i) {
        slot_t *s = &g_slot[i];
        if (!s->url[0] || s->fed) continue;
        char cb[512], cm[512];
        cache_paths(s, cb, sizeof cb, cm, sizeof cm);
        remove(cm);
        s->at = s->tried = 0;                          /* keep showing the old guide meanwhile */
        s->busy = 0;
        ensure(s);
    }
}

const char *evo_epg_user_status(void)
{
    init();
    evo_epg_poll();
    static char buf[48];
    slot_t *s = &g_slot[0];
    if (!s->url[0]) return "NOT SET";
    ensure(s);
    switch (s->state) {
    case ST_READY:
        snprintf(buf, sizeof buf, "%d CHANNELS", s->g ? s->g->nch : 0);
        return buf;
    case ST_LOADING: return "LOADING";
    case ST_FAILED:  return "FAILED";
    case ST_EMPTY:   return "NOT XMLTV";
    default:         return "SET";
    }
}

unsigned evo_epg_generation(void)
{
    return g_gen;
}

void evo_epg_set_provider_source(const char *slot, const char *url)
{
    init();
    slot_t *s = slot_get(slot, url && url[0]);
    if (!s) return;
    if (!url) url = "";
    if (!strcmp(s->url, url) && !s->fed) return;
    slot_reset(s);
    snprintf(s->url, sizeof s->url, "%s", url);
}

int evo_epg_fill_item(evo_provider_item_t *it, const char *slot, const char *epg_id,
                      const char *name)
{
    if (!it) return 0;
    init();
    evo_epg_poll();
    slot_t *user = &g_slot[0];
    slot_t *prov = slot_get(slot, 0);
    ensure(user);
    if (prov) ensure(prov);

    /* The provider's own now/next, already in the item, is its guide too. */
    const int had = it->now_title[0] || it->next_title[0];
    slot_t *order[2];
    int n = 0;
    if (g_prefer_user) {
        order[n++] = user;
        if (!had && prov) order[n++] = prov;
    } else {
        if (had) return 0;
        if (prov) order[n++] = prov;
        order[n++] = user;
    }

    char now_t[sizeof it->now_title], next_t[sizeof it->next_title];
    for (int i = 0; i < n; ++i) {
        const guide_t *g = order[i]->g;
        const int ci = guide_find(g, epg_id, name ? name : it->title);
        if (ci < 0 || !guide_now_next(g, ci, now_t, sizeof now_t, next_t, sizeof next_t))
            continue;
        snprintf(it->now_title, sizeof it->now_title, "%s", now_t);
        snprintf(it->next_title, sizeof it->next_title, "%s", next_t);
        if (now_t[0] && next_t[0])
            snprintf(it->subtitle, sizeof it->subtitle, "%s | Next: %s", now_t, next_t);
        else if (now_t[0])
            snprintf(it->subtitle, sizeof it->subtitle, "%s", now_t);
        else
            snprintf(it->subtitle, sizeof it->subtitle, "Next: %s", next_t);
        return 1;
    }
    return 0;
}

/* ------------------------------------------------------------------------- */
/* Guides a provider downloads itself, and whole programme lists             */
/* ------------------------------------------------------------------------- */

static unsigned body_sig(const char *b, size_t len)
{
    uint32_t h = 2166136261u ^ (uint32_t)len;
    const size_t edge = len < 4096 ? len : 4096;
    for (size_t i = 0; i < edge; ++i) { h ^= (unsigned char)b[i]; h *= 16777619u; }
    for (size_t i = len - edge; i < len; ++i) { h ^= (unsigned char)b[i]; h *= 16777619u; }
    return h ? h : 1;
}

void evo_epg_feed(const char *slot, const char *body, size_t len)
{
    init();
    if (!slot || !body || !len) return;
    slot_t *s = slot_get(slot, 1);
    if (!s) return;
    const unsigned sig = body_sig(body, len);
    if (s->fed && s->fed_sig == sig && (s->g || s->busy)) return;   /* the same guide again */
    char *copy = (char *)malloc(len + 1);
    if (!copy) return;
    memcpy(copy, body, len);
    copy[len] = 0;
    slot_reset(s);
    snprintf(s->url, sizeof s->url, "feed:%s", slot);
    s->fed = 1;
    s->fed_sig = sig;
    job_start(s, JOB_FEED, copy, len, NULL, time(NULL));
}

/* The programmes of channel ci in one guide that overlap [from, to). */
static int guide_programmes(const guide_t *g, int ci, int64_t from, int64_t to,
                            evo_epg_prog_t *out, int max)
{
    if (!g || ci < 0 || ci >= g->nch || max <= 0) return 0;
    const prog_t *pr = g->pr + g->ch[ci].first;
    const int n = g->ch[ci].n;
    int k = 0;
    for (int i = 0; i < n && k < max; ++i) {
        if (pr[i].stop <= from) continue;
        if (pr[i].start >= to) break;
        out[k].start = pr[i].start;
        out[k].stop = pr[i].stop;
        out[k].title = g->pool + pr[i].title;
        out[k].desc = g->pool + pr[i].desc;
        k++;
    }
    return k;
}

int evo_epg_programmes(const char *slot, const char *epg_id, const char *name,
                       int64_t from, int64_t to, evo_epg_prog_t *out, int max)
{
    init();
    evo_epg_poll();
    slot_t *user = &g_slot[0];
    slot_t *prov = slot_get(slot, 0);
    ensure(user);
    if (prov) ensure(prov);
    slot_t *order[2] = { prov, user };
    if (g_prefer_user) { order[0] = user; order[1] = prov; }
    for (int i = 0; i < 2; ++i) {
        if (!order[i] || !order[i]->g) continue;
        const int ci = guide_find(order[i]->g, epg_id, name);
        if (ci < 0 || order[i]->g->ch[ci].n == 0) continue;
        return guide_programmes(order[i]->g, ci, from, to, out, max);
    }
    return 0;
}
