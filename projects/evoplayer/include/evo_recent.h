#ifndef EVO_RECENT_H
#define EVO_RECENT_H

/*
 * Module: evo_recent
 *
 * Recent-files database. Persists to /data/kijkr/evo_recent.txt.
 * Stores the last 25 played files with path, title, position and duration.
 *
 * Public surface:
 *   recent_clear()
 *   recent_add_or_update(path, title, last_pos, duration)
 *   recent_update_current_position()   -- snapshot current playback pos
 *   recent_save()
 *   recent_load()
 *   recent_lookup(path)                -- returns last position, or 0.0
 *   recent_remove_at(index)            -- drop one entry (Continue Watching)
 *   recent_remove(path)                -- same, by path
 *   recent_reset_progress(index)       -- keep the entry, forget the position
 *   recent_clear_all()                 -- empty the list
 *
 * State is exposed read-only via the arrays below for the UI draw code.
 */

#include "evo_data_path.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RECENT_FILE_DB   evo_data_path("evo_recent.txt")
#define MAX_RECENT_FILES 25

typedef struct {
    char      path[512];
    char      title[128];
    double    last_pos;
    double    duration;
    long long last_played;
    char      art[384];      /* poster URL of a provider item, "" for none */
    char      year[8];       /* what Home's big tile says about it, when known */
    char      rating[8];     /* TMDB's score, "7.8", when it came from Home's rows */
    int       ep_total;      /* episodes in its season, 0 = unknown / a film */
    char      overview[256];
} RecentFileEntry;

extern RecentFileEntry recent_files[MAX_RECENT_FILES];
extern int             recent_file_count;
extern int             recent_selected;

void   recent_clear(void);
void   recent_add_or_update(const char *path, const char *title,
                             double last_pos, double duration);
void   recent_update_current_position(void);
void   recent_save(void);
void   recent_load(void);
double recent_lookup(const char *path);   /* returns last_pos or 0.0 */

/* Remove one entry and persist. Returns 1 if something was removed. */
int    recent_remove_at(int index);
int    recent_remove(const char *path);

/* Remember the poster URL of an entry (provider items). Returns 1 if found. */
int    recent_set_art(const char *path, const char *art_url);

/* Remember the year, episode count and description of an entry. 1 if found. */
int    recent_set_info(const char *path, const char *year, int ep_total, const char *overview);

/* Mark an entry watched (its position at its end) and persist. 1 if found. */
int    recent_mark_watched(int index);

/* Drop entries watched to the end more than `days` ago. Returns how many. */
int    recent_prune_watched(int days);

/* Home is about to start this (a film, or a series folder): its poster, for
 * the Continue Watching tile when the portal has none. Kept for the session. */
void   recent_hint_art(const char *path, const char *art_url);
/* ... and its year, score and description (TMDB's), the same way. */
void   recent_hint_info(const char *path, const char *year, const char *rating, const char *overview);

/* Reset the saved position of one entry to 0 and persist. Returns 1 on success. */
int    recent_reset_progress(int index);

/* Empty the whole list and persist. Returns how many entries were dropped. */
int    recent_clear_all(void);

#ifdef __cplusplus
}
#endif

#endif /* EVO_RECENT_H */
