#ifndef EVO_TITLE_TAG_H
#define EVO_TITLE_TAG_H
/*
 * evo_title_tag - make a portal's title fit to show.
 *
 *   "MULTI Scrubs (2026)"      -> "Scrubs (2026)"
 *   "NL - De Luizenmoeder"     -> "De Luizenmoeder"
 *   "|NL| 4K Dune"             -> "Dune"
 *   "【MULTI】 The Bear"        -> "The Bear"
 *   "NL NPO 1 HD ʳᵉᶜ"           -> "NPO 1 HD"
 *
 * Two steps. evo_title_normalize_spaces() turns what the console's font cannot
 * draw (it shows a "?" for each) into plain text: odd spaces, symbols, bars,
 * brackets, emoji and superscript letters become a space or go; curly quotes and
 * dashes become ASCII; letters with accents stay. evo_title_strip_tags() then
 * drops a known label (MULTI, NL, 4K ...) at the start, with whatever wraps it.
 * A title that merely begins with capitals ("NCIS", "IT", "US") is left alone,
 * and a title is never emptied. For display only: keys and grouping keep the
 * stored title.
 */
#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

/* How long the UTF-8 sequence starting at `r` is (1 for anything malformed). */
static inline int evo_title_utf8_len(const unsigned char *r)
{
    if (r[0] < 0x80) return 1;
    int n = (r[0] >= 0xF0) ? 4 : (r[0] >= 0xE0) ? 3 : (r[0] >= 0xC0) ? 2 : 1;
    for (int i = 1; i < n; ++i)
        if ((r[i] & 0xC0) != 0x80) return 1;
    return n;
}

/*
 * In place. Returns nothing; the result is never longer than the input.
 *   -> ' '   : any space-like or symbol character (a separator)
 *   -> ASCII : ‘ ’ “ ” ‐ – — … and full-width letters / digits
 *   -> gone  : zero-width characters, variation selectors, superscript letters
 * Runs of spaces become one and the ends are trimmed.
 */
static inline void evo_title_normalize_spaces(char *s)
{
    if (!s) return;
    unsigned char *r = (unsigned char *)s, *w = (unsigned char *)s;
    int last_space = 1;                                   /* trims the front */
    while (*r) {
        const int n = evo_title_utf8_len(r);
        int space = 0, drop = 0;
        char ascii = 0;
        unsigned cp = 0;
        if (n == 1) {
            cp = r[0];
            if (cp < 0x80) {
                if (cp == ' ' || cp == '\t' || cp == '\r' || cp == '\n') space = 1;
                else if (cp < 0x20 || cp == 0x7F) drop = 1;
            } else {
                drop = 1;                                 /* a stray byte */
            }
        } else {
            if (n == 2) cp = ((r[0] & 0x1Fu) << 6) | (r[1] & 0x3Fu);
            else if (n == 3) cp = ((r[0] & 0x0Fu) << 12) | ((r[1] & 0x3Fu) << 6) | (r[2] & 0x3Fu);
            else cp = ((r[0] & 0x07u) << 18) | ((r[1] & 0x3Fu) << 12) | ((r[2] & 0x3Fu) << 6) | (r[3] & 0x3Fu);

            if (cp == 0x2018 || cp == 0x2019 || cp == 0x201A || cp == 0x2032) ascii = '\'';
            else if (cp == 0x201C || cp == 0x201D || cp == 0x201E) ascii = '"';
            else if (cp >= 0x2010 && cp <= 0x2015) ascii = '-';
            else if (cp >= 0xFF01 && cp <= 0xFF5E) ascii = (char)(cp - 0xFF01 + 0x21);   /* full width */
            else if (cp == 0x00AD || (cp >= 0x200B && cp <= 0x200F) ||
                     (cp >= 0x2060 && cp <= 0x206F) || cp == 0xFEFF ||
                     (cp >= 0xFE00 && cp <= 0xFE0F) || (cp >= 0xE0000))   drop = 1;   /* invisible */
            else if ((cp >= 0x02B0 && cp <= 0x02FF) || (cp >= 0x1D2C && cp <= 0x1DBF) ||
                     (cp >= 0x2070 && cp <= 0x209F))                       drop = 1;   /* superscripts */
            else if ((cp >= 0x0080 && cp <= 0x00BF) || cp == 0x00D7 || cp == 0x00F7 ||
                     (cp >= 0x2000 && cp <= 0x2BFF) || (cp >= 0x3000 && cp <= 0x303F) ||
                     (cp >= 0xFE10 && cp <= 0xFE6F) || (cp >= 0xFFF0 && cp <= 0xFFFF) ||
                     cp >= 0x1F000)                                         space = 1;   /* symbols */
        }
        if (cp == 0x2026) {                               /* "…" -> "..." (same length) */
            w[0] = w[1] = w[2] = '.';
            w += 3; r += n; last_space = 0;
            continue;
        }
        if (drop) { r += n; continue; }
        if (space) {
            if (!last_space) *w++ = ' ';
            last_space = 1;
            r += n;
            continue;
        }
        if (ascii) {
            *w++ = (unsigned char)ascii;
            r += n;
            last_space = 0;
            continue;
        }
        for (int i = 0; i < n; ++i) *w++ = *r++;          /* a letter: keep it */
        last_space = 0;
    }
    while (w > (unsigned char *)s && w[-1] == ' ') w--;
    *w = '\0';
}

static inline int evo_title_tag_is_label(const char *s, size_t n)
{
    static const char *const k[] = {
        "NL", "EN", "DE", "FR", "ES", "TR", "PL", "PT", "BE", "AR",
        "VF", "VFF", "VOSTFR", "VO", "SUB", "DUB", "4K", "UHD", "FHD", "NF", "AMZ", NULL };
    if (n == 5 && strncasecmp(s, "MULTI", 5) == 0) return 1;   /* any case: "Multi", "MULTi" */
    for (int i = 0; k[i]; ++i)
        if (strlen(k[i]) == n && strncmp(s, k[i], n) == 0)
            return 1;
    return 0;
}

/* What may wrap or follow a label: "|NL|", "?MULTI?", "NL -", "[EN]". */
static inline int evo_title_tag_wrap(char c)
{
    return c == ' ' || c == '-' || c == ':' || c == '|' || c == '.' || c == '?' ||
           c == '*' || c == '#' || c == '~' || c == '!' || c == '+' || c == '=' ||
           c == '_' || c == '<' || c == '>' || c == '/' || c == '\\' || c == ',' ||
           c == '[' || c == ']' || c == '{' || c == '}';
}

static inline void evo_title_strip_tags(const char *in, char *out, size_t cap)
{
    if (!out || cap == 0) return;
    if (!in) { out[0] = '\0'; return; }
    char norm[512];
    snprintf(norm, sizeof norm, "%s", in);
    evo_title_normalize_spaces(norm);
    const char *p = norm;
    for (int round = 0; round < 4; ++round) {
        const char *q = p;
        while (*q && evo_title_tag_wrap(*q)) ++q;         /* "|", "[", "?" before it */
        if (*q == '(') {                                  /* "(NL)": a label in brackets */
            size_t n = 0;
            while (isalnum((unsigned char)q[1 + n])) ++n;
            if (n == 0 || q[1 + n] != ')' || !evo_title_tag_is_label(q + 1, n)) break;
            q += n + 2;
        } else {
            size_t n = 0;
            while (q[n] && isalnum((unsigned char)q[n])) ++n;
            if (n == 0 || !evo_title_tag_is_label(q, n)) break;
            /* a label ends at a separator, never inside a word ("NLSeries") */
            if (q[n] && !evo_title_tag_wrap(q[n]) && q[n] != '(') break;
            q += n;
        }
        while (*q && evo_title_tag_wrap(*q)) ++q;         /* "|", "]", "?", " - " after it */
        if (strlen(q) < 2) break;                         /* keep something */
        p = q;
    }
    snprintf(out, cap, "%s", p);
}
#endif /* EVO_TITLE_TAG_H */
