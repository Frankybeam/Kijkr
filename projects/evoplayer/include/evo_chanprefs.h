/*
 * evo_chanprefs - the viewer's own arrangement of live channels: channels
 * hidden from a list, and the order of the channels in a list. Kept per
 * provider ("iptv", "xtream", "stalker") by channel name, so a playlist that
 * is re-sorted or grows keeps the arrangement. Saved in chanprefs.txt.
 */
#ifndef EVO_CHANPREFS_H
#define EVO_CHANPREFS_H

#ifdef __cplusplus
extern "C" {
#endif

/* A channel's name as it is kept: without a leading "★ " and spaces. */
void evo_chanprefs_key(const char *title, char *out, unsigned osz);

int  evo_chanprefs_is_hidden(const char *provider, const char *name);
void evo_chanprefs_set_hidden(const char *provider, const char *name, int hidden);
int  evo_chanprefs_hidden_count(const char *provider);
/* Every hidden channel of `provider` shown again. Returns how many. */
int  evo_chanprefs_unhide_all(const char *provider);

/*
 * The list `parent` of `provider` in the viewer's order: perm[i] is the index
 * into names[] of the channel shown i-th. Channels the saved order does not
 * know come after the ones it does, in the provider's order. Returns 1 when
 * there is a saved order (perm filled), 0 when the list is as the provider
 * sends it (perm filled with 0..n-1).
 */
int  evo_chanprefs_order(const char *provider, const char *parent,
                         const char *const *names, int n, int *perm);
/* names[] (as shown now) with the channel at `from` moved to `to`, kept as
 * the list's order. */
void evo_chanprefs_move(const char *provider, const char *parent,
                        const char *const *names, int n, int from, int to);
/* The list back in the provider's order. */
void evo_chanprefs_reset_order(const char *provider, const char *parent);

#ifdef __cplusplus
}
#endif

#endif
