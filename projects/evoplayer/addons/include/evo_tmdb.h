#ifndef EVO_TMDB_H
#define EVO_TMDB_H

/*
 * Module: evo_tmdb
 *
 * Home's TRENDING, POPULAR FILMS and POPULAR SERIES rows. The lists come from
 * TMDB (themoviedb.org, a free key of the user's own); every title is then
 * looked up in the IPTV portal (EVO_PROVIDER_FIND) and only what the portal
 * has is shown, so pressing a tile plays it from the portal.
 *
 * What a lookup found is kept in tmdb_match.txt (a week; "not there" a day),
 * so the portal is asked about a title once, not every time Home opens.
 *
 * Everything runs on the main thread: evo_tmdb_tick() starts the requests and
 * the replies land from evo_net_poll().
 */

#include "evo_provider.h"

#ifdef __cplusplus
extern "C" {
#endif

enum { EVO_TMDB_TRENDING = 0, EVO_TMDB_POPULAR_FILMS = 1, EVO_TMDB_POPULAR_SERIES = 2,
       EVO_TMDB_NEW_FILMS = 3,      /* released on streaming / disc lately, newest first */
       EVO_TMDB_NEW_SERIES = 4,     /* series with new episodes now */
       EVO_TMDB_LISTS = 5 };
#define EVO_TMDB_MAX_VERSIONS 12
#define EVO_TMDB_VERSIONS_SZ  2400
#define EVO_TMDB_PER_LIST 20
#define EVO_TMDB_KEY_MAX  320

typedef struct {
    int  id;
    char type;                               /* 'v' a film, 's' a series */
    char title[128];
    char original[128];
    char year[8];
    char rating[8];                          /* TMDB's score, "7.8"; "" when too few votes */
    char overview[EVO_PROVIDER_MAX_OVERVIEW];   /* in the TMDB language when it has one */
    char local_title[128];                   /* the title in the TMDB language ("" = as title) */
    char poster[256];                        /* https://image.tmdb.org/t/p/w342/... */
    int  state;                              /* 0 not looked up yet, 1 found somewhere, 2 nowhere */
    char key[EVO_PROVIDER_MAX_ITEM_ID + 32]; /* "evo://stalker/0|vm:123" when state 1 */
    /* Every portal item that is this title ("Dune", "|NL| Dune", "Dune 4K"):
     * key US title RS key US title ... (US 0x1f, RS 0x1e). */
    char versions[EVO_TMDB_VERSIONS_SZ];
    int  folder;                             /* the portal item is a folder (a series) */
    int  attempt;                            /* which search text is next */
    int  tgt;                                /* which source is next (every portal, account, playlist) */
    int  hits;                               /* versions found in that source so far */
} evo_tmdb_item_t;

/* The key: read from tmdb.conf on first use. "" when none. */
const char *evo_tmdb_key(void);
/* Save (or with "" remove) the key. 0 when it has spaces or is too long. */
int  evo_tmdb_set_key(const char *key);

/* Once per frame while Home is up: refresh the lists when due (every six
 * hours) and look the next title up. Every source is searched for every
 * title: each Stalker portal, the Xtream account and the M3U playlist, so
 * all their versions can be chosen from. `provider_id` is unused. */
void evo_tmdb_tick(const char *provider_id);

/* The language of TMDB's titles and stories on Home: 0 follows EVO's own
 * language (Settings > Interface > LANGUAGE), 1..5 English, Nederlands,
 * Deutsch, Français, Español. Kept in tmdb_lang.conf. Matching with the
 * portal always uses the English titles, so a choice never loses versions. */
#define EVO_TMDB_LANG_CHOICES 6
int  evo_tmdb_lang_choice(void);
void evo_tmdb_set_lang_choice(int choice);

/* A list as fetched; Home shows the items whose state is 1. */
int  evo_tmdb_list(int list, const evo_tmdb_item_t **items);

/* How many versions the portal has of it, and version i's key and portal title. */
int  evo_tmdb_version_count(const evo_tmdb_item_t *it);
int  evo_tmdb_version(const evo_tmdb_item_t *it, int i, char *key, size_t key_sz, char *title, size_t title_sz);

/* A problem worth telling the user once ("key not accepted"); 0 when none. */
int  evo_tmdb_take_error(char *out, size_t sz);

/* Exposed for the tests: is a portal title the same as a TMDB title? `year`
 * may be "" (unknown); a film needs the year within one when both know it. */
int  evo_tmdb_same_title(const char *portal_title, const char *portal_year,
                         const char *tmdb_title, const char *tmdb_year, int is_film);

/* Settings > Clear cache: what was found where is forgotten and looked up again. */
void evo_tmdb_clear_cache(void);

/* Forget the lists and every lookup (a new key, another portal). */
void evo_tmdb_reset(void);

#ifdef __cplusplus
}
#endif

#endif /* EVO_TMDB_H */
