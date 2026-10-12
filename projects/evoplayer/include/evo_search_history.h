#ifndef EVO_SEARCH_HISTORY_H
#define EVO_SEARCH_HISTORY_H
/*
 * Module: evo_search_history
 *
 * The last searches, newest first (search_history.txt in EVO's data folder),
 * offered when Search opens. One list for every provider.
 */
#ifdef __cplusplus
extern "C" {
#endif

#define EVO_SHIST_MAX 8

int         evo_shist_count(void);
const char *evo_shist_get(int i);          /* "" when out of range */
void        evo_shist_add(const char *q);  /* to the top; a repeat moves up */
void        evo_shist_remove(int i);
void        evo_shist_clear(void);

#ifdef __cplusplus
}
#endif
#endif
