/*
 * Module: evo_recent
 *
 * Recent-files database. Persists to /data/kijkr/evo_recent.txt.
 * Extracted from main.c (lines 600–614, 1611–1723, 1428–1445).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "evo_recent.h"

/* Shared player globals. */
extern char   current_media_path[768];
extern double media_duration_sec;
extern double resume_base_offset_seconds;

/* Functions from main.c that recent depends on. */
extern long long now_ms(void);
extern double    prospero_media_clock_seconds(void);
extern void      clean_media_title(const char *path, char *title, size_t tsz,
                                   char *meta, size_t msz);

/* --------------------------------------------------------------------------
 * State (definitions — extern declarations are in evo_recent.h)
 * -------------------------------------------------------------------------- */

RecentFileEntry recent_files[MAX_RECENT_FILES];
int             recent_file_count = 0;
int             recent_selected   = 0;

/* --------------------------------------------------------------------------
 * Implementation
 * -------------------------------------------------------------------------- */

void recent_clear(void)
{
    memset(recent_files, 0, sizeof(recent_files));
    recent_file_count = 0;
    recent_selected   = 0;
}

/*
 * "evo://<provider>/<portal>|se:<series>:<season>:<episode>" -> the length of
 * the part every episode of that series shares, or 0 for anything else.
 */
static size_t series_prefix_len(const char *path)
{
    if (strncmp(path, "evo://", 6) != 0) return 0;
    const char *bar = strchr(path + 6, '|');
    if (!bar || (bar[1] != 's' && bar[1] != 'v') || bar[2] != 'e' || bar[3] != ':') return 0;
    const char *last = strrchr(path, ':');
    if (!last || last <= bar + 3) return 0;
    const char *prev = last - 1;
    while (prev > bar + 3 && *prev != ':') prev--;
    if (prev <= bar + 3) return 0;
    return (size_t)(prev - path) + 1;              /* includes the ':' */
}

/*
 * At most this many episodes of one series in the list: the one being watched
 * and the one queued after it. Older episodes only pushed films off the 25
 * places, and Home shows one tile per series anyway.
 */
#define RECENT_EPISODES_PER_SERIES 2

static void prune_series(int keep_index)
{
    const size_t n = series_prefix_len(recent_files[keep_index].path);
    if (n == 0) return;
    char prefix[sizeof recent_files[0].path];
    memcpy(prefix, recent_files[keep_index].path, n);
    prefix[n] = '\0';
    int seen = 0;
    for (int i = 0; i < recent_file_count; i++) {      /* newest first */
        if (series_prefix_len(recent_files[i].path) != n ||
            strncmp(recent_files[i].path, prefix, n) != 0)
            continue;
        if (++seen <= RECENT_EPISODES_PER_SERIES) continue;
        for (int j = i; j < recent_file_count - 1; j++)
            recent_files[j] = recent_files[j + 1];
        recent_file_count--;
        memset(&recent_files[recent_file_count], 0, sizeof(recent_files[0]));
        i--;
    }
}

static const char *hint_for(const char *path);
static void hint_apply_info(RecentFileEntry *e);
static void one_line(char *dst, size_t sz, const char *src);

void recent_add_or_update(const char *path, const char *title,
                           double last_pos, double duration)
{
    if (!path || !path[0]) return;

    int existing = -1;
    for (int i = 0; i < recent_file_count; i++) {
        if (strcmp(recent_files[i].path, path) == 0) {
            existing = i;
            break;
        }
    }

    RecentFileEntry entry;
    memset(&entry, 0, sizeof(entry));
    snprintf(entry.path,  sizeof(entry.path),  "%s", path);
    snprintf(entry.title, sizeof(entry.title), "%s",
             title && title[0] ? title : path);
    entry.last_pos    = last_pos;
    entry.duration    = duration;
    entry.last_played = now_ms();

    if (existing >= 0) {
        /* The poster outlives a replay: keep what the entry already has. */
        snprintf(entry.art, sizeof(entry.art), "%s", recent_files[existing].art);
        snprintf(entry.year, sizeof(entry.year), "%s", recent_files[existing].year);
        snprintf(entry.rating, sizeof(entry.rating), "%s", recent_files[existing].rating);
        entry.ep_total = recent_files[existing].ep_total;
        snprintf(entry.overview, sizeof(entry.overview), "%s", recent_files[existing].overview);
        for (int i = existing; i > 0; i--)
            recent_files[i] = recent_files[i - 1];
        recent_files[0] = entry;
    } else {
        int limit = recent_file_count;
        if (limit >= MAX_RECENT_FILES) limit = MAX_RECENT_FILES - 1;
        for (int i = limit; i > 0; i--)
            recent_files[i] = recent_files[i - 1];
        recent_files[0] = entry;
        if (recent_file_count < MAX_RECENT_FILES)
            recent_file_count++;
    }

    if (!recent_files[0].art[0]) {
        /* An episode reached by "next" may not know its poster: the series'
         * other episodes do. */
        const size_t n = series_prefix_len(recent_files[0].path);
        for (int i = 1; n && i < recent_file_count; i++) {
            if (recent_files[i].art[0] && series_prefix_len(recent_files[i].path) == n &&
                strncmp(recent_files[i].path, recent_files[0].path, n) == 0) {
                snprintf(recent_files[0].art, sizeof(recent_files[0].art), "%s", recent_files[i].art);
                break;
            }
        }
    }
    if (!recent_files[0].art[0]) {
        const char *h = hint_for(recent_files[0].path);     /* started from Home's rows */
        if (h) snprintf(recent_files[0].art, sizeof(recent_files[0].art), "%s", h);
    }
    hint_apply_info(&recent_files[0]);
    prune_series(0);
    recent_selected = 0;
}

/* ---- poster and info hints from Home ---- */
#define HINT_N 16
typedef struct {
    char key[512];
    char art[384];
    char year[8];
    char rating[8];
    char overview[256];
} hint_t;
static hint_t g_hint[HINT_N];
static int    g_hint_next;

static hint_t *hint_slot(const char *path)
{
    for (int i = 0; i < HINT_N; i++)
        if (strcmp(g_hint[i].key, path) == 0) return &g_hint[i];
    hint_t *h = &g_hint[g_hint_next];
    g_hint_next = (g_hint_next + 1) % HINT_N;
    memset(h, 0, sizeof *h);
    snprintf(h->key, sizeof h->key, "%s", path);
    return h;
}

void recent_hint_art(const char *path, const char *art_url)
{
    if (!path || !path[0] || !art_url || strncmp(art_url, "http", 4) != 0) return;
    hint_t *h = hint_slot(path);
    snprintf(h->art, sizeof h->art, "%s", art_url);
}

static void one_line(char *dst, size_t sz, const char *src)
{
    size_t k = 0;
    for (const char *c = src; c && *c && k + 1 < sz; ++c)
        dst[k++] = (*c == '\t' || *c == '\r' || *c == '\n' || *c == '|') ? ' ' : *c;
    dst[k] = '\0';
}

void recent_hint_info(const char *path, const char *year, const char *rating, const char *overview)
{
    if (!path || !path[0]) return;
    hint_t *h = hint_slot(path);
    if (year && year[0])         snprintf(h->year, sizeof h->year, "%.4s", year);
    if (rating && rating[0])     snprintf(h->rating, sizeof h->rating, "%s", rating);
    if (overview && overview[0]) one_line(h->overview, sizeof h->overview, overview);
}

/* The hint for this entry: the same key, or - for an episode - its series. */
static const hint_t *hint_find(const char *path)
{
    for (int i = 0; i < HINT_N; i++) {
        const char *k = g_hint[i].key;
        if (!k[0]) continue;
        if (strcmp(k, path) == 0) return &g_hint[i];
        /* Stalker: "...|ss:200" is the series of "...|se:200:0:4" */
        const char *bar = strrchr(k, '|');
        if (bar && (bar[1] == 's' || bar[1] == 'v') && bar[2] == 's' && bar[3] == ':') {
            char prefix[560];
            snprintf(prefix, sizeof prefix, "%.*s|%ce:%s:", (int)(bar - k), k, bar[1], bar + 4);
            if (strncmp(path, prefix, strlen(prefix)) == 0) return &g_hint[i];
        }
        /* Xtream: "series_show:55" is the series of "series_ep:9001:mkv:55" */
        const char *show = strstr(k, "series_show:");
        if (show && strstr(path, "series_ep:")) {
            const char *sid = show + 12;
            const size_t pl = strlen(path), sl = strlen(sid);
            if (sl && pl > sl + 1 && path[pl - sl - 1] == ':' && strcmp(path + pl - sl, sid) == 0)
                return &g_hint[i];
        }
    }
    return NULL;
}

static const char *hint_for(const char *path)
{
    const hint_t *h = hint_find(path);
    return (h && h->art[0]) ? h->art : NULL;
}

/* Home's year, score and description go over what the portal said. */
static void hint_apply_info(RecentFileEntry *e)
{
    const hint_t *h = hint_find(e->path);
    if (!h) return;
    if (h->year[0])     snprintf(e->year, sizeof e->year, "%s", h->year);
    if (h->rating[0])   snprintf(e->rating, sizeof e->rating, "%s", h->rating);
    if (h->overview[0]) snprintf(e->overview, sizeof e->overview, "%s", h->overview);
}

void recent_update_current_position(void)
{
    if (!current_media_path[0]) return;

    double pos = resume_base_offset_seconds + prospero_media_clock_seconds();
    if (pos < 0.0) pos = 0.0;

    char title[128], meta[96];
    clean_media_title(current_media_path, title, sizeof(title), meta, sizeof(meta));

    recent_add_or_update(current_media_path, title, pos, media_duration_sec);
    recent_save();
}

void recent_save(void)
{
    FILE *fp = fopen(RECENT_FILE_DB, "w");
    if (!fp) return;

    for (int i = 0; i < recent_file_count; i++) {
        fprintf(fp, "%s\t%s\t%.3f\t%.3f\t%lld\t%s",
                recent_files[i].path,
                recent_files[i].title,
                recent_files[i].last_pos,
                recent_files[i].duration,
                recent_files[i].last_played,
                recent_files[i].art);
        /* "i:<year>|<episodes>|<description>": its own marked field, so an
         * empty poster before it (which strtok skips) cannot shift it. */
        /* "j:" adds the score: "j:<year>|<episodes>|<score>|<description>". */
        if (recent_files[i].year[0] || recent_files[i].ep_total > 0 || recent_files[i].overview[0] ||
            recent_files[i].rating[0])
            fprintf(fp, "\tj:%s|%d|%s|%s", recent_files[i].year, recent_files[i].ep_total,
                    recent_files[i].rating, recent_files[i].overview);
        fputc('\n', fp);
    }

    fclose(fp);
}

void recent_load(void)
{
    recent_clear();

    FILE *fp = fopen(RECENT_FILE_DB, "r");
    if (!fp) return;

    char line[2048];
    while (fgets(line, sizeof(line), fp) && recent_file_count < MAX_RECENT_FILES) {
        RecentFileEntry *e = &recent_files[recent_file_count];
        memset(e, 0, sizeof(*e));

        char *path        = strtok(line,  "\t");
        char *title       = strtok(NULL,  "\t");
        char *last_pos    = strtok(NULL,  "\t");
        char *duration    = strtok(NULL,  "\t");
        char *last_played = strtok(NULL,  "\t\r\n");
        char *art         = strtok(NULL,  "\t\r\n");   /* absent in older files */
        char *info        = strtok(NULL,  "\t\r\n");
        if (art && (strncmp(art, "i:", 2) == 0 || strncmp(art, "j:", 2) == 0)) { info = art; art = NULL; }

        if (!path || !title) continue;

        snprintf(e->path,  sizeof(e->path),  "%s", path);
        snprintf(e->title, sizeof(e->title), "%s", title);
        e->last_pos    = last_pos    ? atof(last_pos)    : 0.0;
        e->duration    = duration    ? atof(duration)    : 0.0;
        e->last_played = last_played ? atoll(last_played) : 0;
        if (art) snprintf(e->art, sizeof(e->art), "%s", art);
        if (info && (strncmp(info, "i:", 2) == 0 || strncmp(info, "j:", 2) == 0)) {
            const int with_score = info[0] == 'j';
            const char *y = info + 2;
            const char *b1 = strchr(y, '|');
            const char *b2 = b1 ? strchr(b1 + 1, '|') : NULL;
            const char *b3 = (with_score && b2) ? strchr(b2 + 1, '|') : NULL;
            if (b1 && b2 && (!with_score || b3)) {
                snprintf(e->year, sizeof(e->year), "%.*s", (int)(b1 - y) < 7 ? (int)(b1 - y) : 7, y);
                e->ep_total = atoi(b1 + 1);
                if (with_score) {
                    snprintf(e->rating, sizeof(e->rating), "%.*s",
                             (int)(b3 - b2 - 1) < 7 ? (int)(b3 - b2 - 1) : 7, b2 + 1);
                    snprintf(e->overview, sizeof(e->overview), "%s", b3 + 1);
                } else {
                    snprintf(e->overview, sizeof(e->overview), "%s", b2 + 1);
                }
            }
        }

        recent_file_count++;
    }

    fclose(fp);
}

double recent_lookup(const char *path)
{
    if (!path || !path[0]) return 0.0;
    for (int i = 0; i < recent_file_count; i++) {
        if (strcmp(recent_files[i].path, path) == 0)
            return recent_files[i].last_pos;
    }
    return 0.0;
}

int recent_set_info(const char *path, const char *year, int ep_total, const char *overview)
{
    if (!path || !path[0]) return 0;
    for (int i = 0; i < recent_file_count; i++) {
        RecentFileEntry *e = &recent_files[i];
        if (strcmp(e->path, path) != 0) continue;
        if (year && year[0]) snprintf(e->year, sizeof(e->year), "%.4s", year);
        if (ep_total > 0) e->ep_total = ep_total;
        if (overview && overview[0]) one_line(e->overview, sizeof(e->overview), overview);
        hint_apply_info(e);                           /* Home's TMDB text wins */
        return 1;
    }
    return 0;
}

int recent_mark_watched(int index)
{
    if (index < 0 || index >= recent_file_count) return 0;
    RecentFileEntry *e = &recent_files[index];
    if (e->duration <= 1.0) e->duration = 100.0;      /* never played to a known length */
    e->last_pos = e->duration;
    e->last_played = now_ms();
    recent_save();
    return 1;
}

int recent_prune_watched(int days)
{
    const long long now = now_ms();
    if (days <= 0 || now < 1600000000000LL) return 0;  /* no real clock: keep everything */
    const long long limit = (long long)days * 24LL * 3600LL * 1000LL;
    int dropped = 0;
    for (int i = recent_file_count - 1; i >= 0; i--) {
        const RecentFileEntry *e = &recent_files[i];
        const int done = e->duration > 1.0 && e->last_pos / e->duration >= 0.95;
        if (!done || e->last_played <= 0 || now - e->last_played < limit) continue;
        for (int j = i; j < recent_file_count - 1; j++)
            recent_files[j] = recent_files[j + 1];
        recent_file_count--;
        memset(&recent_files[recent_file_count], 0, sizeof(recent_files[0]));
        dropped++;
    }
    if (dropped) {
        if (recent_selected >= recent_file_count)
            recent_selected = recent_file_count > 0 ? recent_file_count - 1 : 0;
        recent_save();
    }
    return dropped;
}

int recent_remove_at(int index)
{
    if (index < 0 || index >= recent_file_count) return 0;

    for (int i = index; i < recent_file_count - 1; i++)
        recent_files[i] = recent_files[i + 1];

    recent_file_count--;
    memset(&recent_files[recent_file_count], 0, sizeof(recent_files[0]));

    if (recent_selected >= recent_file_count)
        recent_selected = recent_file_count > 0 ? recent_file_count - 1 : 0;

    recent_save();
    return 1;
}

int recent_remove(const char *path)
{
    if (!path || !path[0]) return 0;
    for (int i = 0; i < recent_file_count; i++) {
        if (strcmp(recent_files[i].path, path) == 0)
            return recent_remove_at(i);
    }
    return 0;
}

int recent_set_art(const char *path, const char *art_url)
{
    if (!path || !path[0] || !art_url || !art_url[0]) return 0;
    /* Started from Home's TMDB rows: that poster, not the portal's (often
     * missing or a still from the film). */
    const char *h = hint_for(path);
    if (h) art_url = h;
    for (int i = 0; i < recent_file_count; i++) {
        if (strcmp(recent_files[i].path, path) != 0) continue;
        snprintf(recent_files[i].art, sizeof(recent_files[i].art), "%s", art_url);
        return 1;
    }
    return 0;
}

int recent_reset_progress(int index)
{
    if (index < 0 || index >= recent_file_count) return 0;
    recent_files[index].last_pos = 0.0;
    recent_save();
    return 1;
}

int recent_clear_all(void)
{
    const int dropped = recent_file_count;
    recent_clear();
    recent_save();
    return dropped;
}
