/*
 * provider_stalker.h - what the provider screen needs from the Stalker /
 * Ministra provider beyond the evo_provider_t vtable: hiding and moving the
 * categories of a portal, removing a portal, restoring the lot.
 *
 * Item ids carry the portal's number in front ("2|g:5"); a portal's own folder
 * is "2|".
 */
#ifndef PROVIDER_STALKER_H
#define PROVIDER_STALKER_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define STALKER_ACT_HIDE 0   /* a category: hide it. A portal folder: remove it (press twice). */
#define STALKER_ACT_UP   1   /* a category: one place up */
#define STALKER_ACT_DOWN 2   /* a category: one place down */

/*
 * Returns 0 when something changed (reload the list), 1 when nothing changed
 * (msg says why, or asks for a second press), -1 when item_id is not something
 * these actions apply to. msg is always terminated and may be empty.
 */
int stalker_item_action(const char *item_id, int action, char *msg, size_t msg_sz);

/*
 * L2 / R2 on a channel, film or series: cycle how that kind of list is sorted
 * (dir +1 next, -1 previous). Live: portal order, name A-Z / Z-A, channel number.
 * Movies and series: newest, name A-Z, top rated. 0 = changed (reload the list),
 * -1 = not something that can be sorted. msg names the new order.
 */
int stalker_cycle_sort(const char *item_id, int dir, char *msg, size_t msg_sz);

/*
 * Put a category or a channel at a place of the user's choosing. stalker_move_info
 * gives the 1-based place it has now and how many there are (0), or -1 when the
 * item is not one of the lists currently shown. stalker_move_to moves it: 0 =
 * changed (reload the list), 1 = nothing changed (msg says why). Channels keep
 * their own order per category; categories per kind (live, movies, series).
 */
int stalker_move_info(const char *item_id, int *pos, int *total);
int stalker_move_to(const char *item_id, int new_pos, char *msg, size_t msg_sz);

/*
 * The channel next to the one being watched (dir +1 next, -1 previous; wraps),
 * in the order of the list it was started from. Fills item_id for resolve and
 * the channel's name. 0 = found, -1 = nothing to zap to.
 */
/*
 * Series: the episode before / after the one playing (dir -1 / +1), within the
 * season or into the next / previous season. Fills item_id for resolve and a title.
 * 0 = found, -1 = there is none, -2 = the season next to it has not been opened yet.
 */
int stalker_is_playing_episode(const char *url);
/* Put back which episode is playing (after resolving the next one ahead). */
void stalker_note_playing(const char *item_id, const char *url);
int stalker_episode_neighbor(int dir, char *item_id, size_t id_sz, char *title, size_t title_sz);

/* The title of the episode playing ("Show - S1E3"): gives the series its name
 * when it was read without one (a resume from Home), so the next episode is
 * called "Show - S1E4" rather than "Series - S1E4". */
void stalker_note_title(const char *title);
/* The name a film is known by, given just before it is played (Home): used to
 * look it up on the portal when its kept link no longer works. */
void stalker_hint_title(const char *item_id, const char *title);
/* How many portals are set up (Home looks titles up in each). */
int stalker_portal_count(void);
/* Settings > Clear cache: the category and list pages kept on disk. */
void stalker_clear_cache(void);

/* Changes each time a new TV guide has arrived: the channel list redraws. */
unsigned stalker_epg_generation(void);

/* 1 when this stream URL is the live channel this provider started last. */
int stalker_is_playing(const char *url);

int stalker_zap_neighbor(int dir, char *item_id, size_t id_sz, char *title, size_t title_sz);

/* Show every hidden category again and drop the custom order, for all portals. */
int stalker_restore_categories(char *msg, size_t msg_sz);

#ifdef __cplusplus
}
#endif

#endif /* PROVIDER_STALKER_H */
