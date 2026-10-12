#ifndef EVO_I18N_H
#define EVO_I18N_H

/*
 * Module: evo_i18n
 *
 * The language of EVO's own words (menus, buttons, hints, settings). The
 * screens keep writing English; every line the kit draws goes through
 * evo_tr_n(), which swaps a text that is in the table for the chosen
 * language. A text drawn in capitals ("JUMP BACK IN") comes back in capitals.
 * Film and channel names are never in the table, so they are left alone.
 *
 * The choice is kept in language.conf. A file lang_<code>.txt in EVO's data
 * folder ("English text|Translation" per line) adds to or corrects the table
 * without a new build.
 */

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { EVO_LANG_EN = 0, EVO_LANG_NL, EVO_LANG_DE, EVO_LANG_FR, EVO_LANG_ES, EVO_LANG_COUNT };

int         evo_lang_count(void);
const char *evo_lang_name(int lang);     /* in its own language: "Nederlands" */
const char *evo_lang_code(int lang);     /* "nl" */
int         evo_lang_get(void);          /* reads language.conf the first time */
void        evo_lang_set(int lang);      /* applies and saves */

/* The translation of a whole text, or `en` itself when there is none. */
const char *evo_tr(const char *en);

/* The same for a text that need not end in '\0'. NULL when there is no
 * translation (draw the text as it is); else the translation and its length. */
const char *evo_tr_n(const char *text, size_t len, size_t *out_len);

#ifdef __cplusplus
}
#endif

#endif /* EVO_I18N_H */
