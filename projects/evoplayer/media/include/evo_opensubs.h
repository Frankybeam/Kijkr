/*
 * evo_opensubs.h - search and download subtitles from OpenSubtitles.com.
 *
 * The REST API (v1) wants three things: an API key (Settings -> Subtitles),
 * an account login (download is only allowed for a signed-in user) and a
 * search term. Everything blocking runs on one worker thread; the screen polls
 * evo_osub_state() once a frame and never waits.
 *
 *   evo_osub_search_start()  login (if needed) + search -> OSUB_RESULTS
 *   evo_osub_download_start() download result i         -> OSUB_DONE (a file path)
 *
 * The caller (the subtitle picker) loads the finished file on the UI thread:
 * the cue table is read by the renderer, so the worker never touches it.
 */
#ifndef EVO_OPENSUBS_H
#define EVO_OPENSUBS_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    OSUB_IDLE = 0,
    OSUB_WORKING,     /* a request is running; evo_osub_message() says which */
    OSUB_RESULTS,     /* a result list is ready                              */
    OSUB_DONE,        /* a file was downloaded; evo_osub_file() is its path  */
    OSUB_FAILED       /* evo_osub_message() says why                         */
} evo_osub_state_t;

/* ---- settings (own file: evo_opensubtitles.cfg in the data directory) ---- */
#define EVO_OSUB_LANG_COUNT 8
const char *evo_osub_lang_label(int index);
int         evo_osub_lang_get(void);
int         evo_osub_lang_set(int index);

const char *evo_osub_user(void);                   /* "" when none          */
int         evo_osub_has_password(void);
int         evo_osub_set_user(const char *user);   /* "" clears; 1 on success */
int         evo_osub_set_password(const char *pw); /* "" clears; 1 on success */

/* ---- search / download ---- */
/* media_path may be "" (a provider stream); title_hint is then the search text. */
void evo_osub_search_start(const char *media_path, const char *title_hint);
void evo_osub_download_start(int index);
void evo_osub_reset(void);                         /* drop results, ignore a late reply */

evo_osub_state_t evo_osub_state(void);
const char *evo_osub_message(void);                /* progress or failure text */
int         evo_osub_result_count(void);
void        evo_osub_result_text(int index, char *label, size_t label_cap,
                                 char *detail, size_t detail_cap);
const char *evo_osub_file(void);                   /* path, valid in OSUB_DONE */

/* Exposed for testing: "Show.Name.S02E05.1080p.x264-GRP.mkv" ->
 * query "Show Name", season 2, episode 5. year 0 / season -1 when absent. */
/* The same for a provider's title ("MULTI Scrubs (2026) - S1E2"). */
void evo_osub_parse_title(const char *title, char *query, size_t cap,
                          int *year, int *season, int *episode);

void evo_osub_parse_name(const char *media_path, char *query, size_t cap,
                         int *year, int *season, int *episode);

#ifdef __cplusplus
}
#endif

#endif /* EVO_OPENSUBS_H */
