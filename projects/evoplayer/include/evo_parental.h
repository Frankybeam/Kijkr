#ifndef EVO_PARENTAL_H
#define EVO_PARENTAL_H

/*
 * Module: evo_parental
 *
 * The parental lock. With a PIN set (Settings -> Interface), a locked category
 * opens only after the PIN, and what is in it stays out of search and Home's
 * rows. A category is locked by hand (portal menu -> Lock this category) or,
 * with LOCK ADULT CATEGORIES on, by its name (adult, xxx, 18+, ...). The PIN
 * unlocks for two hours, or until "Lock again".
 *
 * Categories are named by provider and the provider's own id without the
 * portal number: "stalker" + "vc:12", "xtream" + "vod_cat:7". Kept in
 * <data>/parental.txt (the PIN as a hash).
 */

#ifdef __cplusplus
extern "C" {
#endif

int  evo_parental_has_pin(void);
/* Set a new PIN (4 to 8 digits); "" removes it. 0 when not digits. */
int  evo_parental_set_pin(const char *pin);
/* 1 when `pin` is the PIN (always 1 without one). */
int  evo_parental_check(const char *pin);

int  evo_parental_auto(void);
void evo_parental_set_auto(int on);

/* The PIN was typed: everything is open for two hours. 0 = wrong PIN. */
int  evo_parental_unlock(const char *pin);
int  evo_parental_unlocked(void);
void evo_parental_relock(void);

/* A category list was read: an adult name is remembered, so its films stay
 * out of search and Home even when the list is not loaded. */
void evo_parental_note_category(const char *provider, const char *cat_id, const char *name);

/* Locked (by hand or by name), whether or not it is unlocked right now. */
int  evo_parental_category_locked(const char *provider, const char *cat_id, const char *name);
/* Lock / unlock a category by hand. */
void evo_parental_set_locked(const char *provider, const char *cat_id, int locked);

/* Should this be left out right now? (a PIN set, not unlocked, and its
 * category locked - or, with LOCK ADULT CATEGORIES, an adult title). */
int  evo_parental_hides(const char *provider, const char *cat_id, const char *title);

/* An adult-looking name: adult, xxx, porn, 18+, erotic, ... */
int  evo_parental_adult_name(const char *name);

/* Re-read the file (after a restore). */
void evo_parental_reload(void);

#ifdef __cplusplus
}
#endif

#endif /* EVO_PARENTAL_H */
