/*
 * evo_epg - the TV guide (XMLTV) shared by every live-TV provider.
 *
 * Two kinds of guide live here:
 *
 *   - provider guides, one per slot ("xtream"), which a provider points at the
 *     guide its service publishes (Xtream Codes: xmltv.php);
 *   - the user's own guide, set in Settings -> TV GUIDE: an http(s) URL or a
 *     file path (/mnt/usb0/guide.xml).
 *
 * The provider's own guide comes first; the user's guide fills the channels it
 * leaves empty. Settings can turn that round (EVO_EPG_PREFER_USER).
 *
 * Formats: XMLTV as plain .xml or gzip-compressed (.xml.gz, .gz, or a server
 * that sends gzip without saying so - the bytes decide, not the name).
 *
 * The API is main-thread only: downloads go through evo_net and arrive in
 * evo_net_poll(); parsing runs on a worker thread and evo_epg_poll() (called
 * every frame) takes the result. Loading is lazy - the first lookup starts it.
 */
#ifndef EVO_EPG_H
#define EVO_EPG_H

#include <stddef.h>
#include <stdint.h>
#include "evo_provider.h"

#ifdef __cplusplus
extern "C" {
#endif

#define EVO_EPG_MAX_URL 512

/* ---- the user's own guide (Settings) ---------------------------------- */

const char *evo_epg_user_url(void);              /* "" = none */
/* "" removes it. Saved at once, and the new guide starts loading. 0 = rejected. */
int         evo_epg_set_user_url(const char *url);

/* 0: the provider's guide first, the user's fills gaps (default).
 * 1: the user's guide first, the provider's fills gaps. */
int         evo_epg_prefer_user(void);
void        evo_epg_set_prefer_user(int on);

/* Forget the cached copies and download every guide again. */
void        evo_epg_refresh(void);

/* One line for Settings: "NOT SET", "LOADING", "812 CHANNELS", "FAILED". */
const char *evo_epg_user_status(void);

/* Every frame, on the main thread: takes guides the worker finished parsing. */
void        evo_epg_poll(void);

/* Bumps whenever any guide finishes loading - lists re-read now/next on it. */
unsigned    evo_epg_generation(void);

/* ---- provider guides ---------------------------------------------------- */

/* Point a slot at a guide URL ("" clears it). The same URL again is a no-op,
 * so a provider can call this on every list it builds. */
void        evo_epg_set_provider_source(const char *slot, const char *url);

/* ---- lookups ------------------------------------------------------------ */

/*
 * Fill it->now_title / it->next_title for a live channel from the guides, in
 * the order evo_epg_prefer_user() says. `slot` is the provider's slot or NULL,
 * `epg_id` the channel's guide id (tvg-id, epg_channel_id, xmltv_id) or NULL,
 * `name` its display name (matched loosely when the id is unknown).
 *
 * What the provider already put in now_title counts as the provider's guide:
 * it is kept unless the user prefers their own guide and that has the channel.
 * Returns 1 when now/next came from here, and rewrites it->subtitle to match.
 */
int         evo_epg_fill_item(evo_provider_item_t *it, const char *slot,
                              const char *epg_id, const char *name);

/*
 * A guide the provider downloaded itself (the M3U playlist's own guide), handed
 * over whole so the TV guide grid can show its programmes. Parsed on the
 * worker thread like the others; the same body again is a no-op.
 */
void        evo_epg_feed(const char *slot, const char *body, size_t len);

/* One programme. The strings live in the guide: copy them before the next
 * evo_epg_poll() (the next frame). desc is "" when the guide had none. */
typedef struct {
    int64_t     start, stop;
    const char *title;
    const char *desc;
} evo_epg_prog_t;

/*
 * The programmes of one channel overlapping [from, to), oldest first, from
 * the first guide (in evo_epg_prefer_user() order) that has the channel.
 * Matched like evo_epg_fill_item. Returns how many were written.
 */
int         evo_epg_programmes(const char *slot, const char *epg_id, const char *name,
                               int64_t from, int64_t to, evo_epg_prog_t *out, int max);

/* ---- helpers ------------------------------------------------------------ */

/* If buf is gzip, a newly allocated, NUL-terminated inflated copy (free() it)
 * and its length; NULL when buf is not gzip or does not inflate. */
char       *evo_epg_gunzip(const char *buf, size_t len, size_t *out_len);

#ifdef __cplusplus
}
#endif

#endif /* EVO_EPG_H */
