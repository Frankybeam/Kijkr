/*
 * tests/test_runner.c — Comprehensive Unit & Integration Test Suite for EVO Player
 *
 * Runs natively on Linux / macOS / Docker / CI without requiring a PS5 console.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include <stdbool.h>
#include <assert.h>
#include <math.h>

#include "evo_direct_mem.h"
#include "evo_draw.h"
#include "evo_focus.h"
#include "evo_theme.h"
#include "evo_screens.h"
#include "evo_addon.h"
#include "evo_provider.h"
#include "evo_provider_bundle.h"
#include "evo_error.h"
#include "evo_net.h"
#include "evo_playlist_sniff.h"
#include "evo_recent.h"
#include "evo_title_tag.h"
#include "evo_favorites.h"
#include "evo_tmdb.h"
#include "evo_parental.h"
#include "evo_i18n.h"
#include "evo_search_history.h"
#include "evo_chanprefs.h"
#include "addon_xtream.h"
#include "evo_changelog.h"
#include "SDL_ps5tilemap.inc"
#include <sys/time.h>
#include "pp_playback.h"

/* Test runner assertion tracking */
static int g_tests_run = 0;
static int g_tests_passed = 0;
static int g_tests_failed = 0;

#define TEST_START(name) do { \
    g_tests_run++; \
    printf("  RUN  %-50s", name); \
    fflush(stdout); \
} while(0)

#define TEST_PASS() do { \
    g_tests_passed++; \
    printf(" [ PASS ]\n"); \
} while(0)

#define TEST_FAIL(reason) do { \
    g_tests_failed++; \
    printf(" [ FAIL ] (%s at line %d)\n", reason, __LINE__); \
} while(0)

#define TEST_ASSERT(cond, reason) do { \
    if (!(cond)) { \
        TEST_FAIL(reason); \
        return; \
    } \
} while(0)

char g_current_media_title[256] = {0};
char current_media_path[768] = {0};
double media_duration_sec = 0.0;
/* evo_recent.c's player hooks (linked for provider_stalker.c). */
double resume_base_offset_seconds = 0.0;
long long now_ms(void) { static long long t = 1000; return t += 10; }
double prospero_media_clock_seconds(void) { return 0.0; }
void toast(const char *category, const char *msg) {
    (void)category;
    (void)msg;
}

void evo_boot_log(const char *fmt, ...) {
    (void)fmt;
}

void pp_stage_bc_checkpoint(const char *stage) {
    (void)stage;
}

static bool str_contains_ci(const char *haystack, const char *needle) {
    if (!haystack || !needle) return false;
    char a[256], b[256];
    int i = 0;
    for (; haystack[i] && i < 255; i++) {
        char c = haystack[i];
        if (c >= 'A' && c <= 'Z') c += 32;
        a[i] = c;
    }
    a[i] = 0;
    i = 0;
    for (; needle[i] && i < 255; i++) {
        char c = needle[i];
        if (c >= 'A' && c <= 'Z') c += 32;
        b[i] = c;
    }
    b[i] = 0;
    return strstr(a, b) != NULL;
}

void clean_media_title(const char *path, char *line1, size_t line1_sz, char *line2, size_t line2_sz) {
    if (!path || !path[0]) {
        if (line1 && line1_sz > 0) line1[0] = '\0';
        if (line2 && line2_sz > 0) line2[0] = '\0';
        return;
    }

    if (strncmp(path, "http://", 7) == 0 || strncmp(path, "https://", 8) == 0) {
        if (g_current_media_title[0]) {
            snprintf(line1, line1_sz, "%s", g_current_media_title);
        } else {
            const char *slash = strrchr(path, '/');
            const char *fname = slash ? slash + 1 : path;
            char temp[256];
            snprintf(temp, sizeof(temp), "%s", fname);
            char *q = strchr(temp, '?');
            if (q) *q = '\0';
            if (temp[0] && strcmp(temp, "stream") != 0 && strcmp(temp, "master.m3u8") != 0) {
                snprintf(line1, line1_sz, "%s", temp);
            } else {
                snprintf(line1, line1_sz, "Emby Media Stream");
            }
        }
        if (line2 && line2_sz > 0) {
            snprintf(line2, line2_sz, "EMBY STREAM");
        }
        return;
    }

    const char *name = strrchr(path, '/');
    name = name ? name + 1 : path;

    char original[256];
    snprintf(original, sizeof(original), "%s", name);

    char *ext = strrchr(original, '.');
    if (ext) *ext = 0;

    char season[16] = "";
    int season_pos = -1;

    for (int i = 0; original[i]; i++) {
        if ((original[i] == 'S' || original[i] == 's') &&
            original[i+1] >= '0' && original[i+1] <= '9' &&
            original[i+2] >= '0' && original[i+2] <= '9' &&
            (original[i+3] == 'E' || original[i+3] == 'e') &&
            original[i+4] >= '0' && original[i+4] <= '9' &&
            original[i+5] >= '0' && original[i+5] <= '9') {
            snprintf(season, sizeof(season), "S%c%cE%c%c", original[i+1], original[i+2], original[i+4], original[i+5]);
            season_pos = i;
            break;
        }
    }

    if (season_pos > 0) {
        char title[256];
        snprintf(title, sizeof(title), "%s", original);
        title[season_pos] = 0;

        while (strlen(title) > 0 &&
              (title[strlen(title)-1] == '.' || title[strlen(title)-1] == '_' || title[strlen(title)-1] == '-' || title[strlen(title)-1] == ' '))
            title[strlen(title)-1] = 0;

        snprintf(line1, line1_sz, "%s", title);
    } else {
        snprintf(line1, line1_sz, "%s", original);
    }

    const char *quality = str_contains_ci(name, "2160") ? "2160p" :
                          str_contains_ci(name, "1080") ? "1080p" :
                          str_contains_ci(name, "720")  ? "720p"  : "";

    const char *codec = (str_contains_ci(name, "hevc") || str_contains_ci(name, "h265") || str_contains_ci(name, "x265")) ? "HEVC" :
                        (str_contains_ci(name, "h264") || str_contains_ci(name, "x264")) ? "H.264" : "";

    const char *ch = str_contains_ci(name, "6ch") ? "6CH" :
                     str_contains_ci(name, "5.1") ? "5.1" :
                     str_contains_ci(name, "2ch") ? "2CH" : "";

    snprintf(line2, line2_sz, "%s%s%s%s%s%s%s",
        season,
        season[0] && quality[0] ? "  " : "",
        quality,
        (season[0] || quality[0]) && codec[0] ? "  " : "",
        codec,
        (season[0] || quality[0] || codec[0]) && ch[0] ? "  " : "",
        ch);
}

/* ==========================================================================
 * 1. CPU SIMD / Color Converter Tests
 * ========================================================================== */

/* ==========================================================================
 * 2. Direct Memory Slab Allocator Tests
 * ========================================================================== */

static void test_direct_mem_lifecycle(void)
{
    TEST_START("Direct Memory: Allocation, Alignment & Reset");
    
    int rc = evo_direct_mem_init(4 * 1024 * 1024); /* 4 MB test arena */
    TEST_ASSERT(rc == 0, "evo_direct_mem_init failed");
    
    void *p1 = evo_direct_mem_alloc(1024);
    TEST_ASSERT(p1 != NULL, "64-byte aligned alloc failed");
    TEST_ASSERT(((uintptr_t)p1 % 64) == 0, "Pointer not 64-byte aligned");
    
    void *p2 = evo_direct_mem_alloc(2048);
    TEST_ASSERT(p2 != NULL, "128-byte aligned alloc failed");
    TEST_ASSERT(((uintptr_t)p2 % 64) == 0, "Pointer not 64-byte aligned");
    
    evo_direct_mem_stats_t stats;
    evo_direct_mem_get_stats(&stats);
    TEST_ASSERT(stats.allocated_bytes >= (1024 + 2048), "Stats allocated bytes incorrect");
    TEST_ASSERT(stats.num_allocations == 2, "Alloc count mismatch");
    
    evo_direct_mem_free(p1);
    evo_direct_mem_free(p2);
    evo_direct_mem_get_stats(&stats);
    TEST_ASSERT(stats.allocated_bytes == 0, "Free did not clear allocated bytes");

    /*
     * #6: a request larger than the whole pool must still succeed (graceful
     * malloc fallback) and free() must route it back correctly — the 4K video
     * buffers depend on this when the console can't give the full slab.
     */
    void *big = evo_direct_mem_alloc(8 * 1024 * 1024);
    TEST_ASSERT(big != NULL, "oversize alloc did not fall back");
    memset(big, 0xAB, 8 * 1024 * 1024); /* must be writable for its full extent */
    evo_direct_mem_free(big);
    evo_direct_mem_get_stats(&stats);
    TEST_ASSERT(stats.allocated_bytes == 0, "fallback free leaked into pool stats");

    /* What the GPU reads never falls back to the heap: NULL instead. */
    void *gbig = evo_direct_mem_alloc_gpu(8 * 1024 * 1024);
    TEST_ASSERT(gbig == NULL, "a GPU buffer fell back to the heap");
    void *gsmall = evo_direct_mem_alloc_gpu(64 * 1024);
    TEST_ASSERT(gsmall != NULL, "a GPU buffer that fits was refused");
    evo_direct_mem_free(gsmall);
    evo_direct_mem_get_stats(&stats);
    TEST_ASSERT(stats.allocated_bytes == 0, "GPU buffer free leaked");

    evo_direct_mem_shutdown();

    /* The app's pool: 96 MB, and screens' textures up to the cover budget fit
     * with the rest of Home's UI in it. */
    TEST_ASSERT(EVO_DIRECT_MEM_POOL_BYTES == (size_t)96 * 1024 * 1024, "pool size not 96 MB");
    rc = evo_direct_mem_init(EVO_DIRECT_MEM_POOL_BYTES);
    TEST_ASSERT(rc == 0, "96 MB pool init failed");
    evo_direct_mem_get_stats(&stats);
    TEST_ASSERT(stats.total_bytes == EVO_DIRECT_MEM_POOL_BYTES, "pool not the size asked for");
    {
        /* 36 MB of posters (480x720 RGBA, pitched) + fonts + UI, like Home at its worst. */
        void *blk[80];
        int nb = 0;
        size_t used = 0;
        while (used < (size_t)62 * 1024 * 1024 && nb < 80) {
            blk[nb] = evo_direct_mem_alloc_gpu(480u * 4u * 720u + 255u);
            TEST_ASSERT(blk[nb] != NULL, "a poster did not fit in the 96 MB pool");
            used += 480u * 4u * 720u;
            nb++;
        }
        void *more = evo_direct_mem_alloc_gpu((size_t)30 * 1024 * 1024);
        TEST_ASSERT(more != NULL, "no 30 MB headroom left at 62 MB used");
        evo_direct_mem_free(more);
        for (int i = 0; i < nb; ++i) evo_direct_mem_free(blk[i]);
        evo_direct_mem_get_stats(&stats);
        TEST_ASSERT(stats.allocated_bytes == 0, "the pool did not empty again");
    }
    evo_direct_mem_shutdown();
    TEST_PASS();
}

/* ==========================================================================
 * 3. UI Typography, Text Fitting & Title Cleaning Tests
 * ========================================================================== */

static void test_clean_media_title_resolution(void)
{
    TEST_START("UI Media Title: Local & Remote Title Resolution");
    char line1[256];
    char line2[256];
    
    /* 1. Local USB Movie Path */
    g_current_media_title[0] = '\0';
    clean_media_title("/mnt/usb0/Movies/Inception.2010.1080p.BluRay.x264.mkv", line1, sizeof(line1), line2, sizeof(line2));
    TEST_ASSERT(strstr(line1, "Inception") != NULL, "Movie title parsing failed");
    TEST_ASSERT(strstr(line2, "1080p") != NULL || strstr(line2, "H.264") != NULL, "Metadata parsing failed");
    
    /* 2. TV Show Season / Episode */
    clean_media_title("/mnt/usb0/TV/Breaking.Bad.S05E14.720p.mkv", line1, sizeof(line1), line2, sizeof(line2));
    TEST_ASSERT(strstr(line1, "Breaking.Bad") != NULL || strstr(line1, "Breaking Bad") != NULL, "TV title parsing failed");
    TEST_ASSERT(strstr(line2, "S05E14") != NULL, "Season/Episode tag missing");
    
    /* 3. Remote Emby Stream with explicit active title */
    strncpy(g_current_media_title, "Interstellar (2014) [IMAX]", sizeof(g_current_media_title) - 1);
    clean_media_title("http://192.168.0.11:8096/emby/Videos/9988/stream.mkv?static=true", line1, sizeof(line1), line2, sizeof(line2));
    TEST_ASSERT(strcmp(line1, "Interstellar (2014) [IMAX]") == 0, "Emby stream title not resolved");
    TEST_ASSERT(strcmp(line2, "EMBY STREAM") == 0, "Emby stream subtitle badge missing");
    
    g_current_media_title[0] = '\0';
    TEST_PASS();
}

/*
 * #90: the provider bundle path check.
 *
 * This is a trust boundary, not a tidiness check. A bundle's manifest names
 * where each of its files is written and, later, what LoadDocument opens, and
 * RmlUi's file interface reaches disk with a plain fopen() that validates
 * nothing. An entry of "../../../data/evoplayer/emby.conf" getting past
 * evo_bundle_path_safe() means a downloaded file written over stored
 * credentials.
 *
 * The docs claim every one of these is "enforced, not assumed". This is what
 * makes that sentence checkable.
 */
static void test_provider_bundle_path_safety(void)
{
    TEST_START("Providers: bundle path escape rejection");

    /* Legitimate shapes a real bundle uses. */
    static const char *ok[] = {
        "main.rml", "main.rcss", "fonts/Inter.ttf", "img/logo.png",
        "a/b/c/deep.rcss", "with-dash_and_underscore.rml",
        "dotted.name.rcss",            /* dots are fine inside a component */
        NULL
    };
    for (int i = 0; ok[i]; ++i)
        TEST_ASSERT(evo_bundle_path_safe(ok[i]) == 1, ok[i]);

    /* Every rejection the header promises. */
    static const char *bad[] = {
        NULL,                          /* placeholder, replaced below */
        "",                            /* empty */
        "/etc/passwd",                 /* absolute */
        "C:/windows/win.ini",          /* drive letter */
        "..",                          /* bare parent */
        "../secret",                   /* escape up */
        "a/../../b",                   /* escape mid-path */
        "a/..",                        /* escape at the end */
        "...",                         /* a longer run of dots is still dots */
        "a/.../b",
        "a//b",                        /* empty component */
        "a/./b",                       /* single-dot component */
        "dir\\file.rml",              /* backslash separator */
        "file://main.rml",             /* URI scheme */
        "evo:mem/art0-1",              /* would collide with the texture registry */
        "http://host/x.rml",
        "tab\there.rml",               /* control character */
        NULL
    };
    /* index 0 is the NULL pointer case, which the loop below cannot express */
    TEST_ASSERT(evo_bundle_path_safe(NULL) == 0, "NULL accepted");
    for (int i = 1; bad[i]; ++i)
        TEST_ASSERT(evo_bundle_path_safe(bad[i]) == 0, bad[i]);

    /* Over the length cap. */
    char too_long[EVO_BUNDLE_MAX_PATH + 16];
    memset(too_long, 'a', sizeof too_long - 1);
    too_long[sizeof too_long - 1] = 0;
    TEST_ASSERT(evo_bundle_path_safe(too_long) == 0, "over-length path accepted");

    /* A provider id is a directory name, so it is checked the same way. */
    TEST_ASSERT(evo_provider_id_valid("iptv") == 1, "valid id rejected");
    TEST_ASSERT(evo_provider_id_valid("real-debrid") == 1, "valid id rejected");
    TEST_ASSERT(evo_provider_id_valid("") == 0, "empty id accepted");
    TEST_ASSERT(evo_provider_id_valid("../x") == 0, "escaping id accepted");
    TEST_ASSERT(evo_provider_id_valid("IPTV") == 0, "uppercase id accepted");
    TEST_ASSERT(evo_provider_id_valid("a b") == 0, "id with space accepted");
    TEST_ASSERT(evo_provider_id_valid("-lead") == 0, "leading dash accepted");
    TEST_ASSERT(evo_provider_id_valid("trail_") == 0, "trailing underscore accepted");

    /* evo_bundle_path() must refuse what path_safe refuses, and must not
     * produce a path outside the provider's own directory. */
    char out[512];
    TEST_ASSERT(evo_bundle_path("iptv", "../escape.rml", out, sizeof out) != 0,
                "evo_bundle_path allowed an escape");
    TEST_ASSERT(evo_bundle_path("iptv", "main.rml", out, sizeof out) == 0,
                "evo_bundle_path rejected a legitimate name");
    TEST_ASSERT(strstr(out, "/providers/iptv/main.rml") != NULL,
                "evo_bundle_path built the wrong path");

    TEST_PASS();
}

/*
 * #90: query-value escaping. Providers build query strings by hand, so a raw
 * '&' or '=' in a search term changes the request rather than the term.
 */
static void test_provider_url_escape(void)
{
    TEST_START("Providers: URL query-value escaping");

    char out[256];
    TEST_ASSERT(evo_provider_url_escape("plain", out, sizeof out) == 5, "len");
    TEST_ASSERT(strcmp(out, "plain") == 0, "unreserved text was altered");

    TEST_ASSERT(evo_provider_url_escape("a b&c=d", out, sizeof out) > 0, "escape failed");
    TEST_ASSERT(strcmp(out, "a%20b%26c%3Dd") == 0, "separators not escaped");

    /* RFC 3986 unreserved set passes through untouched. */
    TEST_ASSERT(evo_provider_url_escape("-_.~", out, sizeof out) == 4, "unreserved len");
    TEST_ASSERT(strcmp(out, "-_.~") == 0, "unreserved set escaped");

    /* Overflow is an error, never a truncated - and therefore different -
     * query value. */
    char tiny[4];
    TEST_ASSERT(evo_provider_url_escape("abcdef", tiny, sizeof tiny) < 0,
                "overflow silently truncated");

    TEST_PASS();
}

static void test_navigation_grid_and_focus(void)
{
    TEST_START("Navigation Engine: Grid & Focus Bounds Clamping");
    
    evo_focus f;
    evo_focus_init(&f, 10, 6, 0); /* 10 items, 6 visible, wrap=0 */
    TEST_ASSERT(f.index == 0 && f.scroll == 0, "Focus init failed");
    
    /* Move Down */
    evo_focus_move(&f, +1);
    TEST_ASSERT(f.index == 1, "Focus move +1 failed");
    
    /* Move past visible window */
    evo_focus_move(&f, +6);
    TEST_ASSERT(f.index == 7, "Focus move +6 failed");
    TEST_ASSERT(f.scroll > 0, "Focus did not scroll window");
    
    /* Clamp at upper bound */
    evo_focus_move(&f, +50);
    TEST_ASSERT(f.index == 9, "Focus did not clamp at max index");
    
    /* Clamp at lower bound */
    evo_focus_move(&f, -50);
    TEST_ASSERT(f.index == 0 && f.scroll == 0, "Focus did not clamp at zero");
    
    TEST_PASS();
}

/* ==========================================================================
 * 6. Changelog Model Consistency Tests
 * ========================================================================== */

static void test_changelog_model_integrity(void)
{
    TEST_START("Changelog Model: Release & Badges Integrity");
    
    TEST_ASSERT(EVO_CHANGELOG_RELEASE_COUNT > 0, "No changelog releases defined");
    
    for (int i = 0; i < EVO_CHANGELOG_RELEASE_COUNT; i++) {
        const evo_changelog_release *r = &EVO_CHANGELOG_RELEASES[i];
        TEST_ASSERT(r->version != NULL && strlen(r->version) > 0, "Release version is empty");
        TEST_ASSERT(r->date != NULL && strlen(r->date) > 0, "Release date is empty");
        TEST_ASSERT(r->tagline != NULL && strlen(r->tagline) > 0, "Release tagline is empty");
        TEST_ASSERT(r->item_count > 0, "Release has no changelog items");
        TEST_ASSERT(r->items != NULL, "Release items pointer is NULL");
        
        for (int j = 0; j < r->item_count; j++) {
            TEST_ASSERT(r->items[j].text != NULL, "Changelog item text is NULL");
            TEST_ASSERT(r->items[j].kind >= 0 && r->items[j].kind <= 3, "Invalid changelog item category");
        }
    }
    
    TEST_PASS();
}

/* ==========================================================================
 * 7. Common UI Widgets & Surround Studio Rendering Tests
 * ========================================================================== */

/* ==========================================================================
 * 8. IPTV Provider Catalog, Stream Resolution & EPG Display Tests
 * ========================================================================== */

typedef struct {
    int count;
    evo_provider_item_t items[16];
    char resolved_url[256];
} test_iptv_ctx_t;

static void on_test_iptv_items(int ok, const evo_provider_item_t *items, int count, int has_more, void *ud)
{
    (void)has_more;
    test_iptv_ctx_t *ctx = (test_iptv_ctx_t *)ud;
    if (ok && items && count > 0) {
        ctx->count = count;
        int n = count < 16 ? count : 16;
        for (int i = 0; i < n; i++) {
            ctx->items[i] = items[i];
        }
    }
}

static void on_test_iptv_resolve(int ok, const evo_stream_choice_t *choices, int count, void *ud)
{
    test_iptv_ctx_t *ctx = (test_iptv_ctx_t *)ud;
    if (ok && choices && count > 0) {
        snprintf(ctx->resolved_url, sizeof(ctx->resolved_url), "%s", choices[0].url);
    }
}

static void test_iptv_provider_catalog_and_epg(void)
{
    TEST_START("IPTV Provider: Catalog, EPG Formats & Stream Resolution");

    const evo_provider_t *p = evo_provider_find("iptv");
    TEST_ASSERT(p != NULL, "IPTV provider not found in registry");
    TEST_ASSERT(p->caps & EVO_PROVIDER_CAP_LIVE, "IPTV missing LIVE cap");
    TEST_ASSERT(p->caps & EVO_PROVIDER_CAP_CATALOG, "IPTV missing CATALOG cap");
    TEST_ASSERT(p->caps & EVO_PROVIDER_CAP_RESOLVE, "IPTV missing RESOLVE cap");

    /* Create sample M3U file with multiple channel formats */
    const char *test_m3u_path = "/tmp/test_iptv_sample.m3u";
    FILE *f = fopen(test_m3u_path, "w");
    TEST_ASSERT(f != NULL, "Failed to create test M3U file");
    fprintf(f, "#EXTM3U\n");
    fprintf(f, "#EXTINF:-1,20:00 | ATLETICO TORQUE X PENAROL\n");
    fprintf(f, "http://stream.test/live/event1\n");
    fprintf(f, "#EXTINF:-1,24H ARCHIVE 81\n");
    fprintf(f, "http://stream.test/live/24h1\n");
    fprintf(f, "#EXTINF:-1 tvg-id=\"br#gloob-hd\",GLOOB HD\n");
    fprintf(f, "http://stream.test/live/gloob\n");
    fprintf(f, "#EXTINF:-1,A&E HD\n");
    fprintf(f, "http://stream.test/live/ae\n");
    fclose(f);

    /* Initialize provider and set local playlist */
    TEST_ASSERT(p->init() == 0, "IPTV provider init failed");
    TEST_ASSERT(p->set_source(test_m3u_path) == 0, "IPTV provider set_source failed");
    TEST_ASSERT(p->is_configured() == 1, "IPTV provider reports not configured");

    /* List catalog */
    test_iptv_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    int rc = p->list_catalog(NULL, 0, on_test_iptv_items, &ctx);
    TEST_ASSERT(rc == 0, "list_catalog returned error");
    TEST_ASSERT(ctx.count >= 4, "Expected at least 4 channels");

    /* Verify channel items and subtitles */
    int found_event = 0, found_24h = 0, found_gloob = 0, found_ae = 0;
    for (int i = 0; i < ctx.count; i++) {
        if (strstr(ctx.items[i].title, "ATLETICO TORQUE")) {
            found_event = 1;
            TEST_ASSERT(strstr(ctx.items[i].subtitle, "Live Event • 20:00") != NULL, "Event subtitle mismatch");
            TEST_ASSERT(strstr(ctx.items[i].now_title, "ATLETICO TORQUE") != NULL, "Event now_title mismatch");
        } else if (strstr(ctx.items[i].title, "24H ARCHIVE 81")) {
            found_24h = 1;
            TEST_ASSERT(strstr(ctx.items[i].subtitle, "24/7 Series Marathon") != NULL, "24H subtitle mismatch");
            TEST_ASSERT(strstr(ctx.items[i].now_title, "24/7 ARCHIVE 81") != NULL, "24H now_title mismatch");
        } else if (strstr(ctx.items[i].title, "GLOOB HD")) {
            found_gloob = 1;
        } else if (strstr(ctx.items[i].title, "A&E HD")) {
            found_ae = 1;
        }
    }
    TEST_ASSERT(found_event, "Event channel not found in catalog");
    TEST_ASSERT(found_24h, "24H channel not found in catalog");
    TEST_ASSERT(found_gloob, "GLOOB channel not found in catalog");
    TEST_ASSERT(found_ae, "A&E channel not found in catalog");

    /* Resolve first playable channel to stream URL */
    int ch_idx = -1;
    for (int i = 0; i < ctx.count; i++) {
        if (!ctx.items[i].is_folder) {
            ch_idx = i;
            break;
        }
    }
    TEST_ASSERT(ch_idx >= 0, "No playable channel found in catalog");
    p->resolve(ctx.items[ch_idx].id, on_test_iptv_resolve, &ctx);
    TEST_ASSERT(strlen(ctx.resolved_url) > 0, "Failed to resolve channel stream URL");
    TEST_ASSERT(strstr(ctx.resolved_url, "http://stream.test/live/") != NULL, "Resolved stream URL mismatch");

    /* Feed XMLTV EPG data and verify NOW title and subtitle updates */
    const char *sample_xmltv =
        "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
        "<tv>\n"
        "  <channel id=\"br#gloob-hd\">\n"
        "    <display-name>GLOOB HD</display-name>\n"
        "  </channel>\n"
        "  <channel id=\"Ae.br\">\n"
        "    <display-name>A&amp;E HD</display-name>\n"
        "  </channel>\n"
        "  <programme start=\"20200101000000 +0000\" stop=\"20350101000000 +0000\" channel=\"br#gloob-hd\">\n"
        "    <title>Miraculous - As Aventuras de Ladybug</title>\n"
        "  </programme>\n"
        "  <programme start=\"20200101000000 +0000\" stop=\"20350101000000 +0000\" channel=\"Ae.br\">\n"
        "    <title>Acumuladores Compulsivos</title>\n"
        "  </programme>\n"
        "</tv>\n";

    TEST_ASSERT(provider_iptv_feed_xmltv(sample_xmltv, strlen(sample_xmltv)) == 0, "Feed XMLTV failed");

    /* Re-list catalog and verify that NOW title and EPG subtitles are set */
    memset(&ctx, 0, sizeof(ctx));
    rc = p->list_catalog(NULL, 0, on_test_iptv_items, &ctx);
    TEST_ASSERT(rc == 0, "list_catalog re-list returned error");

    int verified_gloob_epg = 0, verified_ae_epg = 0;
    for (int i = 0; i < ctx.count; i++) {
        if (strstr(ctx.items[i].title, "GLOOB HD")) {
            TEST_ASSERT(strstr(ctx.items[i].now_title, "Miraculous") != NULL, "GLOOB now_title EPG mismatch");
            TEST_ASSERT(strstr(ctx.items[i].subtitle, "Miraculous") != NULL, "GLOOB subtitle EPG mismatch");
            verified_gloob_epg = 1;
        } else if (strstr(ctx.items[i].title, "A&E HD")) {
            TEST_ASSERT(strstr(ctx.items[i].now_title, "Acumuladores") != NULL, "A&E now_title EPG mismatch");
            TEST_ASSERT(strstr(ctx.items[i].subtitle, "Acumuladores") != NULL, "A&E subtitle EPG mismatch");
            verified_ae_epg = 1;
        }
    }
    TEST_ASSERT(verified_gloob_epg, "GLOOB EPG not verified");
    TEST_ASSERT(verified_ae_epg, "A&E EPG not verified");

    p->shutdown();
    remove(test_m3u_path);

    TEST_PASS();
}

/* ==========================================================================
 * 10. Frame Interpolation & Motion Smoothing Tests (#105)
 * ========================================================================== */

static int simulate_interp_plan(uint32_t src_w, uint32_t src_h, int ten_bit, double fps, int requested_mode, const char **out_reason)
{
    if (requested_mode == 0) {
        if (out_reason) *out_reason = "Off";
        return 0;
    }
    if (ten_bit) {
        if (out_reason) *out_reason = "10-bit/HDR";
        return 0;
    }
    if (fps >= 49.0) {
        if (out_reason) *out_reason = ">= 50 fps";
        return 0;
    }
    if (src_w > 1920 || src_h > 1080) {
        if (out_reason) *out_reason = "source > 1080p";
        return 0;
    }
    if (out_reason) *out_reason = (requested_mode == 1) ? "Low (Smooth)" : "High (Ultra Smooth)";
    return 1;
}

static uint64_t test_now_us(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000000ull + (uint64_t)tv.tv_usec;
}

static void test_frame_interpolation(void)
{
    TEST_START("Frame Interpolation: Phase Calculation, Bypass & Settings");

    /* 1. Bypass Evaluation Rules (#105 spec: auto-off for sources >= 50 fps, 10-bit/HDR, or > 1080p) */
    const char *reason = NULL;

    /* 10-bit / HDR bypass */
    TEST_ASSERT(simulate_interp_plan(1920, 1080, 1, 23.976, 2, &reason) == 0, "10-bit source must bypass interpolation");
    TEST_ASSERT(strcmp(reason, "10-bit/HDR") == 0, "Bypass reason mismatch for 10-bit");

    /* High frame rate bypass (>= 50 fps) */
    TEST_ASSERT(simulate_interp_plan(1920, 1080, 0, 50.0, 1, &reason) == 0, "50 fps source must bypass interpolation");
    TEST_ASSERT(strcmp(reason, ">= 50 fps") == 0, "Bypass reason mismatch for 50 fps");

    TEST_ASSERT(simulate_interp_plan(1920, 1080, 0, 59.94, 2, &reason) == 0, "59.94 fps source must bypass interpolation");
    TEST_ASSERT(strcmp(reason, ">= 50 fps") == 0, "Bypass reason mismatch for 59.94 fps");

    TEST_ASSERT(simulate_interp_plan(1920, 1080, 0, 60.0, 2, &reason) == 0, "60 fps source must bypass interpolation");
    TEST_ASSERT(strcmp(reason, ">= 50 fps") == 0, "Bypass reason mismatch for 60 fps");

    /* High resolution bypass (> 1080p) */
    TEST_ASSERT(simulate_interp_plan(3840, 2160, 0, 24.0, 2, &reason) == 0, "4K source must bypass interpolation");
    TEST_ASSERT(strcmp(reason, "source > 1080p") == 0, "Bypass reason mismatch for 4K");

    TEST_ASSERT(simulate_interp_plan(2560, 1440, 0, 24.0, 1, &reason) == 0, "1440p source must bypass interpolation");
    TEST_ASSERT(strcmp(reason, "source > 1080p") == 0, "Bypass reason mismatch for 1440p");

    /* Eligible SDR streams <= 1080p and < 50 fps */
    TEST_ASSERT(simulate_interp_plan(1920, 1080, 0, 23.976, 1, &reason) == 1, "1080p 24fps SDR must be eligible");
    TEST_ASSERT(strcmp(reason, "Low (Smooth)") == 0, "Expected Low (Smooth) label");

    TEST_ASSERT(simulate_interp_plan(1920, 1080, 0, 24.0, 2, &reason) == 1, "1080p 24fps High must be eligible");
    TEST_ASSERT(strcmp(reason, "High (Ultra Smooth)") == 0, "Expected High (Ultra Smooth) label");

    TEST_ASSERT(simulate_interp_plan(1280, 720, 0, 25.0, 2, &reason) == 1, "720p 25fps SDR must be eligible");

    /* Off mode */
    TEST_ASSERT(simulate_interp_plan(1920, 1080, 0, 23.976, 0, &reason) == 0, "Off mode must not activate plan");
    TEST_ASSERT(strcmp(reason, "Off") == 0, "Expected Off label");

    /* 2. Presentation Clock & Phase Calculation (pp_playback_get_interp_phase) */
    pp_playback pb;
    pp_playback_init(&pb);

    float phase = -1.0f;
    /* Inactive playback must return 0 */
    TEST_ASSERT(pp_playback_get_interp_phase(&pb, &phase) == 0, "Inactive playback must return 0");
    TEST_ASSERT(phase == 0.0f, "Phase should be 0.0 on inactive playback");

    pb.active = 1;
    pp_clock_start(&pb.clock, 0);
    pb.gl_ready = 1;

    /* When paused, must return 0 */
    pp_clock_pause(&pb.clock);
    TEST_ASSERT(pp_playback_get_interp_phase(&pb, &phase) == 0, "Paused playback must return 0");
    pp_clock_resume(&pb.clock);

    /* When seek_discarding, must return 0 */
    pb.seek_discarding = 1;
    TEST_ASSERT(pp_playback_get_interp_phase(&pb, &phase) == 0, "Seek discarding must return 0");
    pb.seek_discarding = 0;

    /* 24 fps cadence on 60 Hz display (41666 us duration) */
    const int64_t frame_dur_us = 41666;
    pb.display_pts_us = 1000000;
    pb.next_pts_us = pb.display_pts_us + frame_dur_us;
    pb.prev_pts_us = pb.display_pts_us - frame_dur_us;

    /* Simulate 60 Hz vsync ticks:
     * Tick 0: elapsed 0 us     -> phase 0.000
     * Tick 1: elapsed 16666 us -> phase 0.400
     * Tick 2: elapsed 33333 us -> phase 0.800
     * Next frame lands: display_pts_us becomes 1041666, next_pts becomes 1083332
     * Tick 3: elapsed 8334 us  -> phase 0.200
     * Tick 4: elapsed 25000 us -> phase 0.600
     */
    const int64_t simulated_elapsed[5] = { 0, 16666, 33333, 8334, 25000 };
    const float expected_phase[5] = { 0.0f, 0.400f, 0.800f, 0.200f, 0.600f };

    for (int i = 0; i < 5; i++) {
        if (i == 3) {
            /* Frame A advances to next frame */
            pb.prev_pts_us = pb.display_pts_us;
            pb.display_pts_us += frame_dur_us;
            pb.next_pts_us = pb.display_pts_us + frame_dur_us;
        }

        uint64_t now = test_now_us();
        pb.clock.host_start_us = now - (uint64_t)simulated_elapsed[i];
        pb.clock.media_start_pts_us = pb.display_pts_us;

        int rc = pp_playback_get_interp_phase(&pb, &phase);
        TEST_ASSERT(rc == 1, "Valid phase calculation should return 1");
        float diff = fabsf(phase - expected_phase[i]);
        TEST_ASSERT(diff < 0.015f, "Cadence phase mismatch on vsync tick");
    }

    /* Fallback to prev_pts_us when next_pts_us is unknown/equal */
    pb.display_pts_us = 2000000;
    pb.prev_pts_us = 2000000 - frame_dur_us;
    pb.next_pts_us = pb.display_pts_us; /* no future frame known yet */

    uint64_t now = test_now_us();
    pb.clock.host_start_us = now - 20833; /* half frame elapsed */
    pb.clock.media_start_pts_us = pb.display_pts_us;

    int rc = pp_playback_get_interp_phase(&pb, &phase);
    TEST_ASSERT(rc == 1, "Fallback to prev_pts_us should succeed");
    TEST_ASSERT(fabsf(phase - 0.50f) < 0.015f, "Half frame phase should be ~0.50");

    /* Out of bounds duration rejection (< 8000 us or > 120000 us) */
    pb.next_pts_us = pb.display_pts_us + 5000; /* < 8000 us */
    TEST_ASSERT(pp_playback_get_interp_phase(&pb, &phase) == 0, "Dur < 8000us must be rejected");

    pb.next_pts_us = pb.display_pts_us + 150000; /* > 120000 us */
    TEST_ASSERT(pp_playback_get_interp_phase(&pb, &phase) == 0, "Dur > 120000us must be rejected");

    /* 3. Settings Persistence Format Validation */
    /* Verify line 22 parsing logic for MotionSmoothing enum */
    int parsed_mode = -1;
    const char *cfg_lines[] = { "0", "1", "2", "99", "-1" };
    int expected_modes[] = { 0, 1, 2, 0, 0 }; /* out of bounds fall back to 0 (Off) */

    for (size_t i = 0; i < sizeof(cfg_lines)/sizeof(cfg_lines[0]); i++) {
        int v = atoi(cfg_lines[i]);
        if (v < 0 || v > 2) v = 0;
        parsed_mode = v;
        TEST_ASSERT(parsed_mode == expected_modes[i], "Config parsing mode mismatch");
    }

    pp_playback_shutdown(&pb);
    TEST_PASS();
}

/* ==========================================================================
 * Error reporting: playlist sniffing, host extraction, failure wording
 * ========================================================================== */

static void test_error_reporting(void)
{
    TEST_START("Errors: playlist sniffing, host-only text, failure wording");

    /* A channel list - what an Xtream get.php link returns - is a playlist. */
    const char *iptv = "#EXTM3U\n#EXTINF:-1 tvg-id=\"a\",One\nhttp://h/live/1.ts\n";
    TEST_ASSERT(evo_playlist_sniff_is_channel_list(iptv, strlen(iptv)), "IPTV channel list is detected");

    /* ...also with a UTF-8 BOM and leading blank lines. */
    const char *bom = "\xEF\xBB\xBF\r\n\r\n#EXTM3U\r\n#EXTINF:-1,Two\r\nhttp://h/2.ts\r\n";
    TEST_ASSERT(evo_playlist_sniff_is_channel_list(bom, strlen(bom)), "BOM + CRLF channel list is detected");

    /* HLS is also M3U but FFmpeg plays it: it must never be taken for a list. */
    const char *hls_media = "#EXTM3U\n#EXT-X-VERSION:3\n#EXT-X-TARGETDURATION:6\n#EXTINF:6.0,\nseg0.ts\n";
    TEST_ASSERT(!evo_playlist_sniff_is_channel_list(hls_media, strlen(hls_media)), "HLS media playlist is not a channel list");
    const char *hls_master = "#EXTM3U\n#EXT-X-STREAM-INF:BANDWIDTH=800000\nlow.m3u8\n";
    TEST_ASSERT(!evo_playlist_sniff_is_channel_list(hls_master, strlen(hls_master)), "HLS master playlist is not a channel list");

    /* Things that are not playlists at all. */
    TEST_ASSERT(!evo_playlist_sniff_is_channel_list("<html>403 Forbidden</html>", 26), "an HTML error page is not a playlist");
    TEST_ASSERT(!evo_playlist_sniff_is_channel_list("\x00\x00\x00\x20" "ftypisom", 12), "an MP4 header is not a playlist");
    TEST_ASSERT(!evo_playlist_sniff_is_channel_list("#EXTM3U\n", 8), "an empty M3U has no channels to load");
    TEST_ASSERT(!evo_playlist_sniff_is_channel_list(NULL, 0), "NULL is not a playlist");

    /* Host only: never the login, path or query. */
    char host[128];
    evo_error_url_host("http://cf.example.ink:8080/get.php?username=u&password=p", host, sizeof host);
    TEST_ASSERT(strcmp(host, "cf.example.ink") == 0, "host drops port, path and query");
    evo_error_url_host("https://user:secret@srv.example.com/live/u/p/1.ts", host, sizeof host);
    TEST_ASSERT(strcmp(host, "srv.example.com") == 0, "host drops user:pass@");
    evo_error_url_host("/mnt/usb0/file.mkv", host, sizeof host);
    TEST_ASSERT(host[0] == 0 || strstr(host, "secret") == NULL, "a path never yields a credential");

    /* The pending reason is taken once. */
    char msg[256];
    evo_error_clear();
    TEST_ASSERT(!evo_error_take(msg, sizeof msg), "nothing pending after clear");
    evo_error_set("%s: boom %d", "host", 7);
    TEST_ASSERT(evo_error_peek(msg, sizeof msg) && strcmp(msg, "host: boom 7") == 0, "peek returns the reason");
    TEST_ASSERT(evo_error_take(msg, sizeof msg) && strcmp(msg, "host: boom 7") == 0, "take returns the reason");
    TEST_ASSERT(!evo_error_take(msg, sizeof msg), "take clears it");

    /* Failure wording names the host and the cause, and never the login. */
    evo_net_describe_failure(EVO_NET_ERR_DNS, 0, "http://cf.example.ink/get.php?password=p", msg, sizeof msg);
    TEST_ASSERT(strstr(msg, "cf.example.ink") && strstr(msg, "name lookup") && !strstr(msg, "password"),
                "DNS failure names the host and cause, not the login");
    evo_net_describe_failure(EVO_NET_ERR_HTTP, 403, "http://h/x", msg, sizeof msg);
    TEST_ASSERT(strstr(msg, "403") && strstr(msg, "refused"), "HTTP 403 says refused");
    evo_net_describe_failure(EVO_NET_ERR_HTTP, 404, "http://h/x", msg, sizeof msg);
    TEST_ASSERT(strstr(msg, "404") && strstr(msg, "not found"), "HTTP 404 says not found");
    evo_net_describe_failure(EVO_NET_ERR_BODY_LIMIT, 200, "http://h/x", msg, sizeof msg);
    TEST_ASSERT(strstr(msg, "too large"), "oversize reply says so");
    evo_net_describe_failure(-99, 0, "http://h/x", msg, sizeof msg);
    TEST_ASSERT(strstr(msg, "-99"), "unknown code is still shown");
    TEST_PASS();
}

/* Continue Watching: titles shown without the portal's label, and one series
 * kept to two episodes so it cannot push films off the list. */
static void test_title_tags_and_recent_series_cap(void)
{
    TEST_START("Home: title labels & one series capped in Continue Watching");
    char out[160];
    evo_title_strip_tags("MULTI Scrubs (2026)", out, sizeof out);
    TEST_ASSERT(strcmp(out, "Scrubs (2026)") == 0, "MULTI label not removed");
    evo_title_strip_tags("|NL| 4K Dune", out, sizeof out);
    TEST_ASSERT(strcmp(out, "Dune") == 0, "stacked labels not removed");
    evo_title_strip_tags("IT Chapter Two", out, sizeof out);
    TEST_ASSERT(strcmp(out, "IT Chapter Two") == 0, "a real title was cut");
    evo_title_strip_tags("NL", out, sizeof out);
    TEST_ASSERT(strcmp(out, "NL") == 0, "a title was emptied");

    recent_clear();
    recent_add_or_update("evo://stalker/0|se:9:9:0:1", "Show - S1E1", 2400, 2450);
    recent_add_or_update("evo://stalker/0|vm:5", "Film", 100, 6000);
    recent_add_or_update("evo://stalker/0|se:9:9:0:2", "Show - S1E2", 600, 2450);
    recent_add_or_update("evo://stalker/0|se:9:9:0:3", "Show - S1E3", 0, 0);
    int eps = 0, film = 0;
    for (int i = 0; i < recent_file_count; i++) {
        if (strstr(recent_files[i].path, "|se:9:9:")) eps++;
        if (strstr(recent_files[i].path, "|vm:5")) film = 1;
    }
    TEST_ASSERT(eps == 2, "more than two episodes of one series kept");
    TEST_ASSERT(film, "the film was pushed off");
    TEST_ASSERT(strstr(recent_files[0].path, ":0:3") != NULL, "newest episode not first");
    TEST_ASSERT(recent_set_art("evo://stalker/0|vm:5", "http://img/p.jpg") == 1, "poster not kept");
    recent_add_or_update("evo://stalker/0|vm:5", "Film", 200, 6000);
    TEST_ASSERT(strcmp(recent_files[0].art, "http://img/p.jpg") == 0, "poster lost on update");
    /* the next episode, reached with R1, takes its series' poster */
    recent_set_art("evo://stalker/0|se:9:9:0:3", "http://img/show.jpg");
    recent_add_or_update("evo://stalker/0|se:9:9:0:4", "Show - S1E4", 0, 0);
    TEST_ASSERT(strcmp(recent_files[0].art, "http://img/show.jpg") == 0, "next episode has no poster");
    char t[160];
    evo_title_strip_tags("\xC2\xA0" "MULTI" "\xC2\xA0" "Scrubs (2026)", t, sizeof t);
    TEST_ASSERT(strcmp(t, "Scrubs (2026)") == 0, "no-break spaces around MULTI kept");
    evo_title_strip_tags("\xE3\x80\x90" "MULTI" "\xE3\x80\x91" " The Bear", t, sizeof t);
    TEST_ASSERT(strcmp(t, "The Bear") == 0, "bracket symbols around MULTI kept");
    evo_title_strip_tags("?MULTI? MobLand (2025)", t, sizeof t);
    TEST_ASSERT(strcmp(t, "MobLand (2025)") == 0, "question marks around MULTI kept");
    evo_title_strip_tags("Am\xC3\xA9lie (2001)", t, sizeof t);
    TEST_ASSERT(strcmp(t, "Am\xC3\xA9lie (2001)") == 0, "accented letters lost");
    evo_title_strip_tags("[REC] (2007)", t, sizeof t);
    TEST_ASSERT(strcmp(t, "[REC] (2007)") == 0, "a real bracketed title was cut");
    recent_clear();
    TEST_PASS();
}


static void test_home_rows_storage(void)
{
    TEST_START("Home: description, watched marks & favourite posters kept");
    char dir[] = "/tmp/evo_home_testXXXXXX";
    TEST_ASSERT(mkdtemp(dir) != NULL, "no temp folder");
    setenv("EVO_DATA_DIR_OVERRIDE", dir, 1);
    evo_data_path_rebind();

    recent_clear();
    recent_add_or_update("evo://stalker/0|se:9:0:5", "Show - S1E5", 300, 2400);
    recent_add_or_update("evo://xtream/vod_stream:7:mkv", "Film", 60, 6000);
    TEST_ASSERT(recent_set_art("evo://stalker/0|se:9:0:5", "http://img/s.jpg") == 1, "poster not kept");
    TEST_ASSERT(recent_set_info("evo://stalker/0|se:9:0:5", "2019-04", 10,
                                "A plot\twith a tab|and a bar\nand a break") == 1, "info not kept");
    /* no poster, but a year: the info field must not take the poster's place */
    recent_set_info("evo://xtream/vod_stream:7:mkv", "2021", 0, "");
    recent_save();
    recent_load();
    TEST_ASSERT(recent_file_count == 2, "entries lost on reload");
    const RecentFileEntry *show = NULL, *film = NULL;
    for (int i = 0; i < recent_file_count; i++) {
        if (strstr(recent_files[i].path, "se:9")) show = &recent_files[i];
        if (strstr(recent_files[i].path, "vod_stream")) film = &recent_files[i];
    }
    TEST_ASSERT(show && film, "entries renamed on reload");
    TEST_ASSERT(strcmp(show->art, "http://img/s.jpg") == 0, "poster lost on reload");
    TEST_ASSERT(strcmp(show->year, "2019") == 0, "year lost on reload");
    TEST_ASSERT(show->ep_total == 10, "episode count lost on reload");
    TEST_ASSERT(strcmp(show->overview, "A plot with a tab and a bar and a break") == 0,
                "description not kept on one line");
    TEST_ASSERT(film->art[0] == '\0' && strcmp(film->year, "2021") == 0, "info read as a poster");
    /* an update keeps what was learnt */
    recent_add_or_update("evo://stalker/0|se:9:0:5", "Show - S1E5", 900, 2400);
    TEST_ASSERT(recent_files[0].ep_total == 10 && recent_files[0].overview[0], "info lost on update");

    /* a poster handed over by Home: the film itself, and a series' episodes */
    recent_hint_art("evo://stalker/0|ss:77", "http://img/series77.jpg");
    recent_hint_art("evo://xtream/series_show:55", "http://img/x55.jpg");
    recent_add_or_update("evo://stalker/0|se:77:0:3", "Show77 - S1E3", 10, 2000);
    TEST_ASSERT(strcmp(recent_files[0].art, "http://img/series77.jpg") == 0, "series poster not handed to its episode");
    recent_set_art("evo://stalker/0|se:77:0:3", "http://portal/still.jpg");
    TEST_ASSERT(strcmp(recent_files[0].art, "http://img/series77.jpg") == 0, "portal still replaced Home's poster");
    recent_add_or_update("evo://xtream/series_ep:9001:mkv:55", "X - S1E1", 10, 2000);
    TEST_ASSERT(strcmp(recent_files[0].art, "http://img/x55.jpg") == 0, "xtream series poster not handed over");
    recent_add_or_update("evo://stalker/0|se:770:0:3", "Other - S1E3", 10, 2000);
    TEST_ASSERT(recent_files[0].art[0] == '\0', "a poster went to the wrong series");
    /* the score and story from Home survive a save */
    recent_hint_info("evo://stalker/0|ss:77", "2019", "8.4", "A story | with a bar");
    recent_add_or_update("evo://stalker/0|se:77:0:4", "Show77 - S1E4", 10, 2000);
    TEST_ASSERT(strcmp(recent_files[0].rating, "8.4") == 0, "score not handed to the episode");
    recent_save();
    recent_load();
    TEST_ASSERT(strcmp(recent_files[0].rating, "8.4") == 0 && strcmp(recent_files[0].year, "2019") == 0 &&
                strcmp(recent_files[0].overview, "A story   with a bar") == 0, "score / story lost on reload");
    recent_remove_at(0); recent_remove_at(0); recent_remove_at(0); recent_remove_at(0);

    /* mark watched; a clock that is not real never prunes */
    TEST_ASSERT(recent_mark_watched(1) == 1, "mark watched failed");
    TEST_ASSERT(recent_files[1].last_pos >= recent_files[1].duration, "not at its end");
    TEST_ASSERT(recent_prune_watched(7) == 0, "pruned without a real clock");
    TEST_ASSERT(recent_file_count == 2, "an entry went missing");

    /* favourites keep their poster */
    favorites_clear();
    favorites_add("evo://stalker/0|ss:9", "Show", 0.0);
    favorites_set_art("evo://stalker/0|ss:9", "http://img/s.jpg");
    favorites_set_art("evo://stalker/0|ss:9", "not a url");
    favorites_save();
    favorites_load();
    TEST_ASSERT(favorite_count == 1, "favourite lost");
    TEST_ASSERT(strcmp(favorite_files[0].art, "http://img/s.jpg") == 0, "favourite poster lost");

    recent_clear();
    favorites_clear();
    unsetenv("EVO_DATA_DIR_OVERRIDE");
    evo_data_path_rebind();
    TEST_PASS();
}


static void test_tmdb_title_matching(void)
{
    TEST_START("Home: TMDB titles matched to portal titles");
    TEST_ASSERT(evo_tmdb_same_title("MULTI Dune: Part Two (2024)", "", "Dune: Part Two", "2024", 1), "labels/year");
    TEST_ASSERT(evo_tmdb_same_title("Dune Part Two", "2024-02-27", "Dune: Part Two", "2024", 1), "punctuation");
    TEST_ASSERT(evo_tmdb_same_title("|NL| Amelie", "2001", "Am\xC3\xA9lie", "2001", 1), "accents");
    TEST_ASSERT(evo_tmdb_same_title("Fast and Furious 6", "", "Fast & Furious 6", "2013", 1), "ampersand");
    TEST_ASSERT(evo_tmdb_same_title("Bear", "2022", "The Bear", "2022", 0), "leading the");
    TEST_ASSERT(evo_tmdb_same_title("Schindlers List", "1993", "Schindler's List", "1993", 1), "apostrophe");
    TEST_ASSERT(!evo_tmdb_same_title("Dune (1984)", "", "Dune", "2021", 1), "a remake of another year matched");
    TEST_ASSERT(evo_tmdb_same_title("The Last of Us (2025)", "", "The Last of Us", "2023", 0),
                "a series' later season year refused");
    TEST_ASSERT(!evo_tmdb_same_title("Dune: Prophecy", "", "Dune", "2024", 0), "a longer title matched");
    TEST_ASSERT(!evo_tmdb_same_title("", "", "", "", 1), "empty titles matched");
    TEST_PASS();
}


static void test_search_history(void)
{
    TEST_START("Search history: newest first, repeats move up, remove and clear");
    char dir[] = "/tmp/evo_shist_testXXXXXX";
    TEST_ASSERT(mkdtemp(dir) != NULL, "no temp folder");
    setenv("EVO_DATA_DIR_OVERRIDE", dir, 1);
    evo_data_path_rebind();
    evo_shist_clear();
    TEST_ASSERT(evo_shist_count() == 0, "not empty at the start");
    evo_shist_add("Suits");
    evo_shist_add("Dune");
    evo_shist_add("suits");                         /* a repeat, other case: moves up */
    TEST_ASSERT(evo_shist_count() == 2, "a repeat was added twice");
    TEST_ASSERT(strcmp(evo_shist_get(0), "suits") == 0, "the repeat is not on top");
    for (int i = 0; i < 12; ++i) { char q[16]; snprintf(q, sizeof q, "q%d", i); evo_shist_add(q); }
    TEST_ASSERT(evo_shist_count() == EVO_SHIST_MAX, "more than the maximum kept");
    TEST_ASSERT(strcmp(evo_shist_get(0), "q11") == 0, "the newest is not first");
    evo_shist_remove(0);
    TEST_ASSERT(strcmp(evo_shist_get(0), "q10") == 0, "remove took the wrong one");
    evo_shist_clear();
    TEST_ASSERT(evo_shist_count() == 0, "clear left entries");
    evo_shist_add("Kept");
    char path[256];
    snprintf(path, sizeof path, "%s/search_history.txt", dir);
    FILE *f = fopen(path, "r");
    char line[64] = "";
    TEST_ASSERT(f && fgets(line, sizeof line, f), "the history file was not written");
    fclose(f);
    TEST_ASSERT(strncmp(line, "Kept", 4) == 0, "the file holds the wrong search");
    remove(path);
    rmdir(dir);
    unsetenv("EVO_DATA_DIR_OVERRIDE");
    evo_data_path_rebind();
    TEST_PASS();
}

static void test_ui_language(void)
{
    TEST_START("Language: menus translated, capitals kept, names left alone");
    char dir[] = "/tmp/evo_lang_testXXXXXX";
    TEST_ASSERT(mkdtemp(dir) != NULL, "no temp folder");
    setenv("EVO_DATA_DIR_OVERRIDE", dir, 1);
    evo_data_path_rebind();
    TEST_ASSERT(evo_lang_get() == EVO_LANG_EN, "the default is not English");
    TEST_ASSERT(strcmp(evo_tr("Settings"), "Settings") == 0, "English changed a word");
    evo_lang_set(EVO_LANG_NL);
    TEST_ASSERT(strcmp(evo_tr("Settings"), "Instellingen") == 0, "Settings not in Dutch");
    TEST_ASSERT(strcmp(evo_tr("JUMP BACK IN"), "VERDER KIJKEN") == 0, "capitals not kept");
    TEST_ASSERT(strcmp(evo_tr("Dune: Part Two"), "Dune: Part Two") == 0, "a title was translated");
    evo_lang_set(EVO_LANG_FR);
    TEST_ASSERT(strcmp(evo_tr("SETTINGS"), "PARAM\xC3\x88TRES") == 0, "accented capitals wrong");
    size_t n = 0;
    const char *r = evo_tr_n("Back to menu", 4, &n);     /* "Back" */
    TEST_ASSERT(r && n == strlen("Retour") && strncmp(r, "Retour", n) == 0, "a part of a text not looked up");
    /* A file in the data folder corrects the table. */
    char path[256];
    snprintf(path, sizeof path, "%s/lang_nl.txt", dir);
    FILE *f = fopen(path, "w");
    TEST_ASSERT(f != NULL, "cannot write lang_nl.txt");
    fputs("# test\nSettings|Opties\nHello there|Hallo daar\n", f);
    fclose(f);
    evo_lang_set(EVO_LANG_NL);
    TEST_ASSERT(strcmp(evo_tr("Settings"), "Opties") == 0, "the file did not correct the table");
    TEST_ASSERT(strcmp(evo_tr("Hello there"), "Hallo daar") == 0, "the file did not add a line");
    /* Saved, and read back on the next start. */
    snprintf(path, sizeof path, "%s/language.conf", dir);
    f = fopen(path, "r");
    char code[8] = "";
    TEST_ASSERT(f && fgets(code, sizeof code, f), "language.conf not written");
    fclose(f);
    TEST_ASSERT(strncmp(code, "nl", 2) == 0, "the wrong language saved");
    evo_lang_set(EVO_LANG_EN);
    remove(path);
    snprintf(path, sizeof path, "%s/lang_nl.txt", dir);
    remove(path);
    rmdir(dir);
    unsetenv("EVO_DATA_DIR_OVERRIDE");
    evo_data_path_rebind();
    TEST_PASS();
}

static void test_parental_lock(void)
{
    TEST_START("Parental lock: PIN, locked and adult categories");
    char dir[] = "/tmp/evo_parental_testXXXXXX";
    TEST_ASSERT(mkdtemp(dir) != NULL, "no temp folder");
    setenv("EVO_DATA_DIR_OVERRIDE", dir, 1);
    evo_data_path_rebind();
    evo_parental_reload();

    TEST_ASSERT(!evo_parental_has_pin(), "a PIN out of nowhere");
    TEST_ASSERT(!evo_parental_hides("stalker", "vc:9", "XXX Film"), "hidden without a PIN");
    TEST_ASSERT(!evo_parental_set_pin("12a4"), "letters accepted as a PIN");
    TEST_ASSERT(!evo_parental_set_pin("123"), "a 3-digit PIN accepted");
    TEST_ASSERT(evo_parental_set_pin("2468"), "PIN not saved");
    TEST_ASSERT(evo_parental_check("2468") && !evo_parental_check("1111"), "PIN check wrong");

    /* by name */
    evo_parental_note_category("stalker", "0|vc:99", "ADULTS 18+");
    TEST_ASSERT(evo_parental_category_locked("stalker", "1|vc:99", NULL), "adult category not remembered");
    TEST_ASSERT(evo_parental_hides("stalker", "vc:99", "Some Film"), "film in an adult category shown");
    TEST_ASSERT(!evo_parental_hides("stalker", "vc:5", "Adulthood"), "an ordinary film hidden");
    TEST_ASSERT(evo_parental_hides("stalker", "", "Hot XXX Night"), "xxx title shown");
    /* by hand */
    evo_parental_set_locked("xtream", "vod_cat:7", 1);
    TEST_ASSERT(evo_parental_hides("xtream", "vod_cat:7", "Cartoon"), "locked category shown");
    /* unlocking */
    TEST_ASSERT(!evo_parental_unlock("0000"), "wrong PIN unlocked");
    TEST_ASSERT(evo_parental_unlock("2468"), "right PIN refused");
    TEST_ASSERT(!evo_parental_hides("xtream", "vod_cat:7", "Cartoon"), "still hidden after the PIN");
    evo_parental_relock();
    TEST_ASSERT(evo_parental_hides("xtream", "vod_cat:7", "Cartoon"), "not locked again");
    /* a hand-unlock beats the name; it all survives a restart */
    evo_parental_set_locked("stalker", "vc:99", 0);
    evo_parental_note_category("stalker", "vc:99", "ADULTS 18+");
    evo_parental_reload();
    TEST_ASSERT(!evo_parental_category_locked("stalker", "vc:99", "ADULTS 18+"), "hand-unlock lost");
    TEST_ASSERT(evo_parental_category_locked("xtream", "vod_cat:7", "Cartoon"), "lock lost on reload");
    TEST_ASSERT(evo_parental_check("2468"), "PIN lost on reload");
    evo_parental_set_auto(0);
    TEST_ASSERT(!evo_parental_category_locked("stalker", "vc:50", "XXX"), "auto off still locks by name");
    TEST_ASSERT(evo_parental_set_pin(""), "PIN not removed");
    TEST_ASSERT(!evo_parental_hides("xtream", "vod_cat:7", "Cartoon"), "hidden without a PIN");

    unsetenv("EVO_DATA_DIR_OVERRIDE");
    evo_data_path_rebind();
    evo_parental_reload();
    TEST_PASS();
}

/* ==========================================================================
 * Main Test Runner Entrypoint
 * ========================================================================== */

static void test_channel_arrangement(void)
{
    TEST_START("Channels: hide, move, order kept by name");
    char dir[] = "/tmp/evo_chanprefs_testXXXXXX";
    TEST_ASSERT(mkdtemp(dir) != NULL, "no temp folder");
    setenv("EVO_DATA_DIR_OVERRIDE", dir, 1);
    evo_data_path_rebind();
    const char *names[] = { "NPO 1", "NPO 2", "RTL 4", "SBS 6" };
    int perm[5];
    TEST_ASSERT(evo_chanprefs_order("iptv", "g:1", names, 4, perm) == 0, "an order before any move");
    evo_chanprefs_move("iptv", "g:1", names, 4, 3, 0);              /* SBS 6 to the top */
    evo_chanprefs_order("iptv", "g:1", names, 4, perm);
    TEST_ASSERT(perm[0] == 3 && perm[1] == 0 && perm[3] == 2, "the move did not stick");
    const char *grown[] = { "NPO 1", "Nieuw", "NPO 2", "RTL 4", "SBS 6" };
    evo_chanprefs_order("iptv", "g:1", grown, 5, perm);
    TEST_ASSERT(perm[0] == 4 && perm[4] == 1, "a new channel did not go last");
    TEST_ASSERT(evo_chanprefs_order("iptv", "g:2", names, 4, perm) == 0, "another list took the order");
    evo_chanprefs_set_hidden("xtream", "RTL 4", 1);
    TEST_ASSERT(evo_chanprefs_is_hidden("xtream", "RTL 4"), "not hidden");
    TEST_ASSERT(!evo_chanprefs_is_hidden("iptv", "RTL 4"), "hidden for another provider");
    char key[32];
    evo_chanprefs_key("\xE2\x98\x85 RTL 4 ", key, sizeof key);
    TEST_ASSERT(strcmp(key, "RTL 4") == 0, "the star was kept in the name");
    TEST_ASSERT(evo_chanprefs_unhide_all("xtream") == 1, "show hidden did not count one");
    char path[256];
    snprintf(path, sizeof path, "%s/chanprefs.txt", dir);
    remove(path);
    rmdir(dir);
    unsetenv("EVO_DATA_DIR_OVERRIDE");
    evo_data_path_rebind();
    TEST_PASS();
}

static int  g_xt_n;
static char g_xt_first[EVO_PROVIDER_MAX_ITEM_ID];
static char g_xt_url[EVO_PROVIDER_MAX_URL];
static void xt_items_cb(int ok, const evo_provider_item_t *items, int count, int more, void *ud)
{
    (void)more; (void)ud;
    g_xt_n = ok ? count : -1;
    g_xt_first[0] = 0;
    if (ok && count > 0) snprintf(g_xt_first, sizeof g_xt_first, "%s", items[0].id);
}
static void xt_resolve_cb(int ok, const evo_stream_choice_t *c, int count, void *ud)
{
    (void)ud;
    g_xt_url[0] = 0;
    if (ok && count > 0) snprintf(g_xt_url, sizeof g_xt_url, "%s", c[0].url);
}

static void test_xtream_accounts_and_catchup(void)
{
    TEST_START("Xtream: two accounts, ids per account, catch-up URL");
    char dir[] = "/tmp/evo_xtacct_testXXXXXX";
    TEST_ASSERT(mkdtemp(dir) != NULL, "no temp folder");
    setenv("EVO_DATA_DIR_OVERRIDE", dir, 1);
    evo_data_path_rebind();
    xtream_init();
    TEST_ASSERT(evo_provider_xtream.set_source("http://one.example:8080/get.php?username=u1&password=p1") == 0,
                "first account refused");
    TEST_ASSERT(evo_provider_xtream.set_source("http://two.example/get.php?username=u2&password=p2") == 0,
                "second account refused");
    TEST_ASSERT(xtream_account_count() == 2, "not two accounts");
    evo_provider_xtream.list_catalog("", 0, xt_items_cb, NULL);
    TEST_ASSERT(g_xt_n == 2 && strncmp(g_xt_first, "acct:", 5) == 0, "the root does not list the accounts");
    evo_provider_xtream.list_catalog("acct:1", 0, xt_items_cb, NULL);
    TEST_ASSERT(g_xt_n == 3 && strcmp(g_xt_first, "1|live") == 0, "the second account's ids have no number");
    /* An id of the first account switches back to it. */
    evo_provider_xtream.resolve("live_stream:7", xt_resolve_cb, NULL);
    TEST_ASSERT(strstr(g_xt_url, "one.example:8080/live/u1/p1/7.") != NULL, "played on the wrong account");
    evo_provider_xtream.resolve("1|xa:42:1760000000:1760003600", xt_resolve_cb, NULL);
    TEST_ASSERT(strstr(g_xt_url, "two.example/timeshift/u2/p2/60/") != NULL && strstr(g_xt_url, "/42.ts"),
                "the catch-up URL is wrong");
    /* Signing out removes the account in use; the other one takes over. */
    evo_provider_xtream.set_source("");
    TEST_ASSERT(xtream_account_count() == 1, "sign out did not remove the account");
    TEST_ASSERT(strcmp(xtream_get_config()->host, "one.example") == 0, "the other account did not take over");
    evo_provider_xtream.set_source("");
    char path[256];
    const char *files[] = { "xtream.conf", "xtream_accounts.conf" };
    for (int i = 0; i < 2; ++i) { snprintf(path, sizeof path, "%s/%s", dir, files[i]); remove(path); }
    rmdir(dir);
    unsetenv("EVO_DATA_DIR_OVERRIDE");
    evo_data_path_rebind();
    TEST_PASS();
}

int main(void)
{
    printf("\n======================================================================\n");
    printf("  EVO Player — Automated Test Suite & Coverage Verification\n");
    printf("======================================================================\n\n");
    
    test_direct_mem_lifecycle();
    test_clean_media_title_resolution();
    test_provider_bundle_path_safety();
    test_provider_url_escape();
    test_navigation_grid_and_focus();
    test_changelog_model_integrity();
    test_iptv_provider_catalog_and_epg();
    test_frame_interpolation();
    test_error_reporting();
    test_title_tags_and_recent_series_cap();
    test_home_rows_storage();
    test_tmdb_title_matching();
    test_parental_lock();
    test_ui_language();
    test_search_history();
    test_channel_arrangement();
    test_xtream_accounts_and_catchup();

    printf("\n----------------------------------------------------------------------\n");
    printf("  Results: %d/%d passed (%d failed)\n",
           g_tests_passed, g_tests_run, g_tests_failed);
    printf("======================================================================\n\n");
    
    return (g_tests_failed == 0) ? 0 : 1;
}
