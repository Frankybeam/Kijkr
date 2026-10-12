/*
 * Module: evo_chanprefs - see include/evo_chanprefs.h.
 *
 * chanprefs.txt, one line each:
 *   h <TAB> provider <TAB> name                  a hidden channel
 *   o <TAB> provider <TAB> parent <TAB> name     the next channel of a list's order
 */
#include "evo_chanprefs.h"
#include "evo_data_path.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CP_FILE      "chanprefs.txt"
#define CP_MAX_LISTS 64
#define CP_MAX_NAMES 3000

typedef struct {
    char  *provider;
    char  *parent;
    char **names;
    int    n;
} cp_list_t;

static char    **g_hidden;      /* "provider\tname" */
static int       g_hidden_n, g_hidden_cap;
static cp_list_t g_lists[CP_MAX_LISTS];
static int       g_lists_n;
static int       g_loaded;

static char *dup_str(const char *s)
{
    size_t n = strlen(s);
    char *d = (char *)malloc(n + 1);
    if (d) memcpy(d, s, n + 1);
    return d;
}

void evo_chanprefs_key(const char *title, char *out, unsigned osz)
{
    if (!out || !osz) return;
    const char *s = title ? title : "";
    while (*s == ' ') s++;
    if (strncmp(s, "\xE2\x98\x85", 3) == 0) s += 3;          /* ★ */
    while (*s == ' ') s++;
    snprintf(out, osz, "%s", s);
    size_t n = strlen(out);
    while (n && out[n - 1] == ' ') out[--n] = 0;
}

static void hidden_push(const char *provider, const char *name)
{
    if (g_hidden_n == g_hidden_cap) {
        int cap = g_hidden_cap ? g_hidden_cap * 2 : 32;
        char **nh = (char **)realloc(g_hidden, (size_t)cap * sizeof *nh);
        if (!nh) return;
        g_hidden = nh;
        g_hidden_cap = cap;
    }
    size_t n = strlen(provider) + strlen(name) + 2;
    char *e = (char *)malloc(n);
    if (!e) return;
    snprintf(e, n, "%s\t%s", provider, name);
    g_hidden[g_hidden_n++] = e;
}

static int hidden_find(const char *provider, const char *name)
{
    const size_t pl = strlen(provider);
    for (int i = 0; i < g_hidden_n; ++i)
        if (strncmp(g_hidden[i], provider, pl) == 0 && g_hidden[i][pl] == '\t' &&
            strcmp(g_hidden[i] + pl + 1, name) == 0)
            return i;
    return -1;
}

static cp_list_t *list_find(const char *provider, const char *parent, int create)
{
    for (int i = 0; i < g_lists_n; ++i)
        if (strcmp(g_lists[i].provider, provider) == 0 && strcmp(g_lists[i].parent, parent) == 0)
            return &g_lists[i];
    if (!create || g_lists_n >= CP_MAX_LISTS) return NULL;
    cp_list_t *l = &g_lists[g_lists_n];
    memset(l, 0, sizeof *l);
    l->provider = dup_str(provider);
    l->parent = dup_str(parent);
    if (!l->provider || !l->parent) { free(l->provider); free(l->parent); return NULL; }
    g_lists_n++;
    return l;
}

static void list_clear(cp_list_t *l)
{
    for (int i = 0; i < l->n; ++i) free(l->names[i]);
    free(l->names);
    l->names = NULL;
    l->n = 0;
}

static void list_push(cp_list_t *l, const char *name)
{
    if (l->n >= CP_MAX_NAMES) return;
    if ((l->n & 63) == 0) {
        char **nn = (char **)realloc(l->names, (size_t)(l->n + 64) * sizeof *nn);
        if (!nn) return;
        l->names = nn;
    }
    char *d = dup_str(name);
    if (d) l->names[l->n++] = d;
}

static void load(void)
{
    if (g_loaded) return;
    g_loaded = 1;
    FILE *f = fopen(evo_data_path(CP_FILE), "r");
    if (!f) return;
    char line[1024];
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        char *f1 = strchr(line, '\t');
        if (!f1) continue;
        *f1++ = 0;
        char *f2 = strchr(f1, '\t');
        if (!f2) continue;
        *f2++ = 0;
        if (strcmp(line, "h") == 0) {
            if (*f2 && hidden_find(f1, f2) < 0) hidden_push(f1, f2);
        } else if (strcmp(line, "o") == 0) {
            char *f3 = strchr(f2, '\t');
            if (!f3) continue;
            *f3++ = 0;
            cp_list_t *l = list_find(f1, f2, 1);
            if (l && *f3) list_push(l, f3);
        }
    }
    fclose(f);
}

static void save(void)
{
    char tmp[600];
    snprintf(tmp, sizeof tmp, "%s.tmp", evo_data_path(CP_FILE));
    FILE *f = fopen(tmp, "w");
    if (!f) return;
    for (int i = 0; i < g_hidden_n; ++i) fprintf(f, "h\t%s\n", g_hidden[i]);
    for (int i = 0; i < g_lists_n; ++i)
        for (int j = 0; j < g_lists[i].n; ++j)
            fprintf(f, "o\t%s\t%s\t%s\n", g_lists[i].provider, g_lists[i].parent, g_lists[i].names[j]);
    const int ok = fclose(f) == 0;
    if (!ok || rename(tmp, evo_data_path(CP_FILE)) != 0) remove(tmp);
}

static int safe(const char *s)
{
    return s && *s && !strpbrk(s, "\t\r\n");
}

int evo_chanprefs_is_hidden(const char *provider, const char *name)
{
    if (!provider || !name) return 0;
    load();
    return g_hidden_n > 0 && hidden_find(provider, name) >= 0;
}

void evo_chanprefs_set_hidden(const char *provider, const char *name, int hidden)
{
    if (!safe(provider) || !safe(name)) return;
    load();
    const int at = hidden_find(provider, name);
    if (hidden && at < 0) hidden_push(provider, name);
    else if (!hidden && at >= 0) {
        free(g_hidden[at]);
        memmove(&g_hidden[at], &g_hidden[at + 1], (size_t)(g_hidden_n - at - 1) * sizeof *g_hidden);
        g_hidden_n--;
    } else {
        return;
    }
    save();
}

int evo_chanprefs_hidden_count(const char *provider)
{
    if (!provider) return 0;
    load();
    const size_t pl = strlen(provider);
    int n = 0;
    for (int i = 0; i < g_hidden_n; ++i)
        if (strncmp(g_hidden[i], provider, pl) == 0 && g_hidden[i][pl] == '\t') n++;
    return n;
}

int evo_chanprefs_unhide_all(const char *provider)
{
    if (!provider) return 0;
    load();
    const size_t pl = strlen(provider);
    int k = 0, gone = 0;
    for (int i = 0; i < g_hidden_n; ++i) {
        if (strncmp(g_hidden[i], provider, pl) == 0 && g_hidden[i][pl] == '\t') {
            free(g_hidden[i]);
            gone++;
        } else {
            g_hidden[k++] = g_hidden[i];
        }
    }
    g_hidden_n = k;
    if (gone) save();
    return gone;
}

typedef struct { const char *name; int idx; } cp_ix_t;

static int ix_cmp(const void *a, const void *b)
{
    const cp_ix_t *x = (const cp_ix_t *)a, *y = (const cp_ix_t *)b;
    const int c = strcmp(x->name, y->name);
    return c ? c : (x->idx - y->idx);
}

int evo_chanprefs_order(const char *provider, const char *parent,
                        const char *const *names, int n, int *perm)
{
    if (!perm || n <= 0) return 0;
    for (int i = 0; i < n; ++i) perm[i] = i;
    if (!provider || !names) return 0;
    load();
    const cp_list_t *l = list_find(provider, parent ? parent : "", 0);
    if (!l || l->n == 0) return 0;

    /* The saved names, sorted for lookup: name -> rank. */
    cp_ix_t *ix = (cp_ix_t *)malloc((size_t)l->n * sizeof *ix);
    int *rank = (int *)malloc((size_t)n * sizeof *rank);
    char *used = (char *)calloc((size_t)l->n, 1);
    if (!ix || !rank || !used) { free(ix); free(rank); free(used); return 0; }
    for (int i = 0; i < l->n; ++i) { ix[i].name = l->names[i]; ix[i].idx = i; }
    qsort(ix, (size_t)l->n, sizeof *ix, ix_cmp);
    for (int i = 0; i < n; ++i) {
        rank[i] = -1;
        if (!names[i]) continue;
        /* The first saved place of this name not taken yet (two channels may
         * share a name). */
        int lo = 0, hi = l->n;
        while (lo < hi) {
            const int mid = (lo + hi) / 2;
            if (strcmp(ix[mid].name, names[i]) < 0) lo = mid + 1; else hi = mid;
        }
        for (int j = lo; j < l->n && strcmp(ix[j].name, names[i]) == 0; ++j)
            if (!used[j]) { used[j] = 1; rank[i] = ix[j].idx; break; }
    }
    /* Known ones by rank, then the rest as they came. */
    int *at = (int *)malloc((size_t)l->n * sizeof *at);
    if (!at) { free(ix); free(rank); free(used); return 0; }
    for (int r = 0; r < l->n; ++r) at[r] = -1;
    for (int i = 0; i < n; ++i)
        if (rank[i] >= 0) at[rank[i]] = i;
    int k = 0;
    for (int r = 0; r < l->n; ++r)
        if (at[r] >= 0) perm[k++] = at[r];
    for (int i = 0; i < n; ++i)
        if (rank[i] < 0) perm[k++] = i;
    free(at);
    free(ix); free(rank); free(used);
    return 1;
}

void evo_chanprefs_move(const char *provider, const char *parent,
                        const char *const *names, int n, int from, int to)
{
    if (!safe(provider) || !names || n <= 0 || from < 0 || from >= n) return;
    if (to < 0) to = 0;
    if (to >= n) to = n - 1;
    if (strpbrk(parent ? parent : "", "\t\r\n")) return;
    load();
    cp_list_t *l = list_find(provider, parent ? parent : "", 1);
    if (!l) return;
    list_clear(l);
    for (int i = 0; i < n; ++i) {
        int src = i;
        if (from < to) { if (i >= from && i < to) src = i + 1; else if (i == to) src = from; }
        else if (from > to) { if (i > to && i <= from) src = i - 1; else if (i == to) src = from; }
        if (safe(names[src])) list_push(l, names[src]);
    }
    save();
}

void evo_chanprefs_reset_order(const char *provider, const char *parent)
{
    if (!provider) return;
    load();
    cp_list_t *l = list_find(provider, parent ? parent : "", 0);
    if (!l || l->n == 0) return;
    list_clear(l);
    save();
}
