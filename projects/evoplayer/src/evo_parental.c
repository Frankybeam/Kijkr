/*
 * Module: evo_parental - see include/evo_parental.h.
 */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "evo_parental.h"
#include "evo_data_path.h"

/* Case-insensitive "does hay contain needle". The console's libc leaves
 * strcasestr as a NULL import: calling it crashes (see evo_webui.c). */
static int pl_ci_contains(const char *hay, const char *needle)
{
    if (!hay || !needle) return 0;
    const size_t n = strlen(needle);
    if (n == 0) return 1;
    for (; *hay; ++hay)
        if (strncasecmp(hay, needle, n) == 0) return 1;
    return 0;
}

#define PARENTAL_FILE  "parental.txt"
#define UNLOCK_SECONDS (2 * 3600)
#define MAX_KEYS       256

static int    g_loaded;
static char   g_pin_hash[24];              /* "" = no PIN */
static int    g_auto = 1;                  /* adult names lock themselves */
static char   g_lock[MAX_KEYS][96];        /* locked by hand */
static int    g_lock_n;
static char   g_adult[MAX_KEYS][96];       /* adult by name, seen in a category list */
static int    g_adult_n;
static char   g_open[MAX_KEYS][96];        /* unlocked by hand although the name says adult */
static int    g_open_n;
static time_t g_unlocked_until;

static void pin_hash(const char *pin, char *out, size_t sz)
{
    unsigned long long h = 1469598103934665603ull;
    const char *salt = "evo-parental|";
    for (const char *c = salt; *c; ++c) h = (h ^ (unsigned char)*c) * 1099511628211ull;
    for (const char *c = pin; *c; ++c) h = (h ^ (unsigned char)*c) * 1099511628211ull;
    snprintf(out, sz, "%016llx", h);
}

static void make_key(const char *provider, const char *cat_id, char *out, size_t sz)
{
    /* "0|vc:12" -> "vc:12": a category is the same on every portal number. */
    const char *bar = cat_id ? strchr(cat_id, '|') : NULL;
    snprintf(out, sz, "%s/%s", provider ? provider : "", bar ? bar + 1 : (cat_id ? cat_id : ""));
}

static int in_list(char (*list)[96], int n, const char *key)
{
    for (int i = 0; i < n; ++i)
        if (strcmp(list[i], key) == 0) return i;
    return -1;
}

static void save(void)
{
    FILE *f = fopen(evo_data_path(PARENTAL_FILE), "w");
    if (!f) return;
    fprintf(f, "pin=%s\nauto=%d\n", g_pin_hash, g_auto);
    for (int i = 0; i < g_lock_n; ++i) fprintf(f, "lock=%s\n", g_lock[i]);
    for (int i = 0; i < g_adult_n; ++i) fprintf(f, "adult=%s\n", g_adult[i]);
    for (int i = 0; i < g_open_n; ++i) fprintf(f, "open=%s\n", g_open[i]);
    fclose(f);
}

static void load(void)
{
    if (g_loaded) return;
    g_loaded = 1;
    g_pin_hash[0] = '\0';
    g_auto = 1;
    g_lock_n = g_adult_n = g_open_n = 0;
    FILE *f = fopen(evo_data_path(PARENTAL_FILE), "r");
    if (!f) return;
    char line[160];
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = '\0';
        if (strncmp(line, "pin=", 4) == 0)       snprintf(g_pin_hash, sizeof g_pin_hash, "%s", line + 4);
        else if (strncmp(line, "auto=", 5) == 0) g_auto = atoi(line + 5) != 0;
        else if (strncmp(line, "lock=", 5) == 0 && g_lock_n < MAX_KEYS)
            snprintf(g_lock[g_lock_n++], sizeof g_lock[0], "%s", line + 5);
        else if (strncmp(line, "adult=", 6) == 0 && g_adult_n < MAX_KEYS)
            snprintf(g_adult[g_adult_n++], sizeof g_adult[0], "%s", line + 6);
        else if (strncmp(line, "open=", 5) == 0 && g_open_n < MAX_KEYS)
            snprintf(g_open[g_open_n++], sizeof g_open[0], "%s", line + 5);
    }
    fclose(f);
}

void evo_parental_reload(void)
{
    g_loaded = 0;
    load();
}

int evo_parental_has_pin(void)
{
    load();
    return g_pin_hash[0] != '\0';
}

int evo_parental_set_pin(const char *pin)
{
    load();
    if (!pin || !pin[0]) {
        g_pin_hash[0] = '\0';
        save();
        return 1;
    }
    const size_t n = strlen(pin);
    if (n < 4 || n > 8) return 0;
    for (size_t i = 0; i < n; ++i)
        if (!isdigit((unsigned char)pin[i])) return 0;
    pin_hash(pin, g_pin_hash, sizeof g_pin_hash);
    g_unlocked_until = 0;
    save();
    return 1;
}

int evo_parental_check(const char *pin)
{
    load();
    if (!g_pin_hash[0]) return 1;
    char h[24];
    pin_hash(pin ? pin : "", h, sizeof h);
    return strcmp(h, g_pin_hash) == 0;
}

int evo_parental_auto(void)
{
    load();
    return g_auto;
}

void evo_parental_set_auto(int on)
{
    load();
    g_auto = on ? 1 : 0;
    save();
}

int evo_parental_unlock(const char *pin)
{
    if (!evo_parental_check(pin)) return 0;
    g_unlocked_until = time(NULL) + UNLOCK_SECONDS;
    return 1;
}

int evo_parental_unlocked(void)
{
    load();
    return !g_pin_hash[0] || time(NULL) < g_unlocked_until;
}

void evo_parental_relock(void)
{
    g_unlocked_until = 0;
}

/* A film's own title: only the unmistakable words ("Adulthood" is a film). */
static int adult_title(const char *name)
{
    if (!name || !name[0]) return 0;
    static const char *const words[] = { "xxx", "porn", "hentai", "[18+]", "(18+)" };
    for (size_t i = 0; i < sizeof words / sizeof words[0]; ++i)
        if (pl_ci_contains(name, words[i])) return 1;
    return 0;
}

int evo_parental_adult_name(const char *name)
{
    if (!name || !name[0]) return 0;
    static const char *const words[] = { "adult", "xxx", "porn", "erotic", "18+", "+18", "(18)",
                                         "[18]", "for adults", "hentai", "playboy", "hustler" };
    for (size_t i = 0; i < sizeof words / sizeof words[0]; ++i)
        if (pl_ci_contains(name, words[i])) return 1;
    return 0;
}

void evo_parental_note_category(const char *provider, const char *cat_id, const char *name)
{
    load();
    if (!evo_parental_adult_name(name)) return;
    char key[96];
    make_key(provider, cat_id, key, sizeof key);
    if (in_list(g_adult, g_adult_n, key) >= 0 || g_adult_n >= MAX_KEYS) return;
    snprintf(g_adult[g_adult_n++], sizeof g_adult[0], "%s", key);
    save();
}

int evo_parental_category_locked(const char *provider, const char *cat_id, const char *name)
{
    load();
    if (!cat_id || !cat_id[0]) return 0;
    char key[96];
    make_key(provider, cat_id, key, sizeof key);
    if (in_list(g_lock, g_lock_n, key) >= 0) return 1;
    if (!g_auto || in_list(g_open, g_open_n, key) >= 0) return 0;
    return in_list(g_adult, g_adult_n, key) >= 0 || evo_parental_adult_name(name);
}

void evo_parental_set_locked(const char *provider, const char *cat_id, int locked)
{
    load();
    char key[96];
    make_key(provider, cat_id, key, sizeof key);
    const int at = in_list(g_lock, g_lock_n, key);
    const int op = in_list(g_open, g_open_n, key);
    if (locked) {
        if (at < 0 && g_lock_n < MAX_KEYS) snprintf(g_lock[g_lock_n++], sizeof g_lock[0], "%s", key);
        if (op >= 0) {
            memmove(&g_open[op], &g_open[op + 1], sizeof g_open[0] * (size_t)(g_open_n - op - 1));
            g_open_n--;
        }
    } else {
        if (op < 0 && g_open_n < MAX_KEYS) snprintf(g_open[g_open_n++], sizeof g_open[0], "%s", key);
        if (at >= 0) {
            memmove(&g_lock[at], &g_lock[at + 1], sizeof g_lock[0] * (size_t)(g_lock_n - at - 1));
            g_lock_n--;
        }
        /* Unlocking an adult-named one by hand: it stops being adult as well. */
        const int ad = in_list(g_adult, g_adult_n, key);
        if (ad >= 0) {
            memmove(&g_adult[ad], &g_adult[ad + 1], sizeof g_adult[0] * (size_t)(g_adult_n - ad - 1));
            g_adult_n--;
        }
    }
    save();
}

int evo_parental_hides(const char *provider, const char *cat_id, const char *title)
{
    if (evo_parental_unlocked()) return 0;          /* also: no PIN at all */
    if (cat_id && cat_id[0] && evo_parental_category_locked(provider, cat_id, NULL)) return 1;
    return g_auto && adult_title(title);
}
