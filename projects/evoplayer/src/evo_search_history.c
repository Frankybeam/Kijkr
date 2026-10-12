/*
 * Module: evo_search_history - see include/evo_search_history.h.
 */
#include "evo_search_history.h"
#include "evo_data_path.h"

#include <stdio.h>
#include <string.h>

#define SHIST_FILE "search_history.txt"

static char g_q[EVO_SHIST_MAX][128];
static int  g_n;
static int  g_loaded;

static void load(void)
{
    if (g_loaded) return;
    g_loaded = 1;
    g_n = 0;
    FILE *f = fopen(evo_data_path(SHIST_FILE), "r");
    if (!f) return;
    char line[160];
    while (g_n < EVO_SHIST_MAX && fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = '\0';
        if (line[0]) snprintf(g_q[g_n++], sizeof g_q[0], "%s", line);
    }
    fclose(f);
}

static void save(void)
{
    FILE *f = fopen(evo_data_path(SHIST_FILE), "w");
    if (!f) return;
    for (int i = 0; i < g_n; ++i) fprintf(f, "%s\n", g_q[i]);
    fclose(f);
}

int evo_shist_count(void)
{
    load();
    return g_n;
}

const char *evo_shist_get(int i)
{
    load();
    return (i >= 0 && i < g_n) ? g_q[i] : "";
}

static int same_ci(const char *a, const char *b)
{
    for (; *a && *b; ++a, ++b) {
        char x = *a, y = *b;
        if (x >= 'A' && x <= 'Z') x = (char)(x - 'A' + 'a');
        if (y >= 'A' && y <= 'Z') y = (char)(y - 'A' + 'a');
        if (x != y) return 0;
    }
    return *a == *b;
}

void evo_shist_add(const char *q)
{
    load();
    if (!q || !q[0] || strchr(q, '\n')) return;
    char item[128];
    snprintf(item, sizeof item, "%s", q);
    int at = -1;
    for (int i = 0; i < g_n; ++i)
        if (same_ci(g_q[i], item)) { at = i; break; }
    if (at < 0) at = g_n < EVO_SHIST_MAX ? g_n++ : EVO_SHIST_MAX - 1;
    for (int i = at; i > 0; --i) memcpy(g_q[i], g_q[i - 1], sizeof g_q[0]);
    memcpy(g_q[0], item, sizeof item);
    save();
}

void evo_shist_remove(int i)
{
    load();
    if (i < 0 || i >= g_n) return;
    for (int k = i; k + 1 < g_n; ++k) memcpy(g_q[k], g_q[k + 1], sizeof g_q[0]);
    g_n--;
    save();
}

void evo_shist_clear(void)
{
    g_loaded = 1;
    g_n = 0;
    remove(evo_data_path(SHIST_FILE));
}
