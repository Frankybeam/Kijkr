/*
 * Module: evo_i18n - see include/evo_i18n.h.
 */
#include "evo_i18n.h"
#include "evo_data_path.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LANG_CONF "language.conf"

typedef struct {
    const char *text[EVO_LANG_COUNT];
} row_t;

static const row_t k_rows[] = {
#include "evo_i18n_table.inc"
};
#define N_ROWS ((int)(sizeof k_rows / sizeof k_rows[0]))

static const char *const k_names[EVO_LANG_COUNT] = { "English", "Nederlands", "Deutsch", "Français", "Español" };
static const char *const k_codes[EVO_LANG_COUNT] = { "en", "nl", "de", "fr", "es" };

/* What the current language says, plain and in capitals; and the extra lines
 * from lang_<code>.txt. */
#define MAX_EXTRA 512
#define SLOTS     2048                  /* open addressing; a power of two */

typedef struct {
    const char *en;
    const char *tr;
    char       *upper;                  /* heap, made when the language is set */
} entry_t;

static entry_t g_entry[N_ROWS + MAX_EXTRA];
static int     g_entry_n;
static int     g_slot[SLOTS];           /* entry index + 1; 0 = empty */
static char   *g_extra_buf;             /* lang_<code>.txt, kept while it is in use */
static int     g_lang = -1;

int evo_lang_count(void) { return EVO_LANG_COUNT; }

const char *evo_lang_name(int lang)
{
    return (lang >= 0 && lang < EVO_LANG_COUNT) ? k_names[lang] : k_names[0];
}

const char *evo_lang_code(int lang)
{
    return (lang >= 0 && lang < EVO_LANG_COUNT) ? k_codes[lang] : k_codes[0];
}

static unsigned hash_ci(const char *s, size_t n)
{
    unsigned h = 2166136261u;
    for (size_t i = 0; i < n; ++i) h = (h ^ (unsigned char)tolower((unsigned char)s[i])) * 16777619u;
    return h;
}

static int eq_ci(const char *a, size_t an, const char *b)
{
    size_t i = 0;
    for (; i < an && b[i]; ++i)
        if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i])) return 0;
    return i == an && b[i] == '\0';
}

/* Capitals of a UTF-8 text: ASCII, and the Latin-1 letters (é -> É). */
static char *upper_dup(const char *s)
{
    size_t n = strlen(s);
    char *o = (char *)malloc(n + 1);
    if (!o) return NULL;
    for (size_t i = 0; i < n; ++i) {
        unsigned char c = (unsigned char)s[i];
        if (c == 0xC3 && i + 1 < n) {
            unsigned char d = (unsigned char)s[i + 1];
            o[i] = (char)c;
            o[i + 1] = (char)((d >= 0xA0 && d <= 0xBE && d != 0xB7) ? d - 0x20 : d);
            ++i;
            continue;
        }
        o[i] = (char)toupper(c);
    }
    o[n] = '\0';
    return o;
}

static void clear_entries(void)
{
    for (int i = 0; i < g_entry_n; ++i) free(g_entry[i].upper);
    memset(g_entry, 0, sizeof g_entry);
    memset(g_slot, 0, sizeof g_slot);
    g_entry_n = 0;
    free(g_extra_buf);
    g_extra_buf = NULL;
}

static void put(const char *en, const char *tr)
{
    if (!en || !en[0] || !tr || !tr[0]) return;
    const size_t n = strlen(en);
    unsigned h = hash_ci(en, n) & (SLOTS - 1);
    for (int probe = 0; probe < SLOTS; ++probe, h = (h + 1) & (SLOTS - 1)) {
        if (!g_slot[h]) {
            if (g_entry_n >= N_ROWS + MAX_EXTRA) return;
            entry_t *e = &g_entry[g_entry_n];
            e->en = en;
            e->tr = tr;
            e->upper = upper_dup(tr);
            g_slot[h] = ++g_entry_n;
            return;
        }
        entry_t *e = &g_entry[g_slot[h] - 1];
        if (eq_ci(en, n, e->en)) {                     /* the file corrects the table */
            free(e->upper);
            e->tr = tr;
            e->upper = upper_dup(tr);
            return;
        }
    }
}

static void load_extra(int lang)
{
    char leaf[32];
    snprintf(leaf, sizeof leaf, "lang_%s.txt", k_codes[lang]);
    FILE *f = fopen(evo_data_path(leaf), "rb");
    if (!f) return;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > 256 * 1024) { fclose(f); return; }
    g_extra_buf = (char *)malloc((size_t)sz + 1);
    if (!g_extra_buf) { fclose(f); return; }
    size_t got = fread(g_extra_buf, 1, (size_t)sz, f);
    fclose(f);
    g_extra_buf[got] = '\0';
    int added = 0;
    for (char *line = g_extra_buf; line && *line && added < MAX_EXTRA; ) {
        char *next = strchr(line, '\n');
        if (next) *next++ = '\0';
        size_t l = strlen(line);
        while (l && (line[l - 1] == '\r' || line[l - 1] == ' ')) line[--l] = '\0';
        char *bar = strchr(line, '|');
        if (line[0] != '#' && bar && bar != line && bar[1]) {
            *bar = '\0';
            put(line, bar + 1);
            added++;
        }
        line = next;
    }
}

static void apply(int lang)
{
    clear_entries();
    g_lang = lang;
    if (lang == EVO_LANG_EN) {
        /* English: only the language names, so "FRANÇAIS" keeps its Ç. */
        for (int i = 0; i < N_ROWS; ++i)
            if (strcmp(k_rows[i].text[1], k_rows[i].text[0]) == 0) put(k_rows[i].text[0], k_rows[i].text[0]);
        load_extra(lang);
        return;
    }
    for (int i = 0; i < N_ROWS; ++i) put(k_rows[i].text[0], k_rows[i].text[lang]);
    load_extra(lang);
}

int evo_lang_get(void)
{
    if (g_lang >= 0) return g_lang;
    int lang = EVO_LANG_EN;
    FILE *f = fopen(evo_data_path(LANG_CONF), "r");
    if (f) {
        char code[16] = "";
        if (fgets(code, sizeof code, f)) {
            code[strcspn(code, "\r\n ")] = '\0';
            for (int i = 0; i < EVO_LANG_COUNT; ++i)
                if (strcmp(code, k_codes[i]) == 0) lang = i;
        }
        fclose(f);
    }
    apply(lang);
    return g_lang;
}

void evo_lang_set(int lang)
{
    if (lang < 0 || lang >= EVO_LANG_COUNT) lang = EVO_LANG_EN;
    apply(lang);
    FILE *f = fopen(evo_data_path(LANG_CONF), "w");
    if (f) {
        fprintf(f, "%s\n", k_codes[lang]);
        fclose(f);
    }
}

const char *evo_tr_n(const char *text, size_t len, size_t *out_len)
{
    if (g_lang < 0) evo_lang_get();
    if (!text || len < 2 || g_entry_n == 0) return NULL;
    unsigned h = hash_ci(text, len) & (SLOTS - 1);
    for (int probe = 0; probe < SLOTS; ++probe, h = (h + 1) & (SLOTS - 1)) {
        if (!g_slot[h]) return NULL;
        const entry_t *e = &g_entry[g_slot[h] - 1];
        if (!eq_ci(text, len, e->en)) continue;
        /* Capitals in, capitals out ("JUMP BACK IN"); otherwise as written. */
        int lower = 0, letters = 0;
        for (size_t i = 0; i < len; ++i) {
            const char ch = text[i];                  /* ASCII only, no locale tables */
            if (ch >= 'a' && ch <= 'z') { lower++; letters++; }
            else if (ch >= 'A' && ch <= 'Z') letters++;
        }
        const char *r = (letters >= 2 && lower == 0 && e->upper) ? e->upper : e->tr;
        if (out_len) *out_len = strlen(r);
        return r;
    }
    return NULL;
}

const char *evo_tr(const char *en)
{
    if (!en) return en;
    const char *r = evo_tr_n(en, strlen(en), NULL);
    return r ? r : en;
}
