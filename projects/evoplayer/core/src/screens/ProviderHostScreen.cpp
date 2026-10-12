#include "evo/screens/ProviderHostScreen.hpp"
#include "evo/Application.hpp"
#include "evo/screens/ScreenManager.hpp"
#include "evo/services/PlaybackController.hpp"

#include "evo_rmlui_provider.h"
#include "evo_rmlui_bridge.h"   /* evo_rmlui_render_nav_overlay */
#include "evo_feedback.h"
#include "evo_keyboard.h"
#include "evo_toast.h"
#include "evo_webui.h"      /* #101: web-UI providers open in the system browser */
#include "evo_nav.h"        /* EVO_SECTION_EMBY - the rail slot the chooser lights */

extern "C" {
#include "evo_provider.h"
#include "evo_recent.h"                            /* watched / not watched */
#include "provider_stalker.h"                       /* hide / move categories, restore */
#include "evo_boot_trace.h"
#include "evo_readdir.h"
#include "evo_favorites.h"
#include "evo_parental.h"
#include "evo_search_history.h"
#include "evo_chanprefs.h"                         /* hide / move channels */
#include "evo_epg.h"                              /* the TV guide grid */
#include "addon_xtream.h"                         /* remove an account */
#include "evo_stream_io.h"                          /* evo_stream_headers */
#include "evo_hls_variants.h"                    /* the stream picker's quality list */
#include "evo_net.h"                               /* evo_net_discover_* */
#include "evo/interfaces/ISettingsService.hpp"
}

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <ctime>
#include <string>
#include <vector>
#include <cctype>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <pthread.h>

namespace evo {

namespace {

/* Set by the launch tile / rail before navigating here. Empty means "whichever
 * provider is enabled", which is what the single shared rail slot does. */
std::string g_pendingProvider;

/* A provider item to start as soon as its provider is open: set by
 * resumeFromRecent(), consumed by startSelected(), dropped by onExit(). */
struct LaunchResume {
    bool        active = false;
    std::string provider;
    std::string item_id;
    std::string title;
    long long   resume_sec = 0;
    bool        is_live = false;     /* a channel (the TV guide grid's "watch") */
};
LaunchResume g_launchResume;

/* The TV guide grid over a channel list (see "The TV guide grid" below). */
struct GridChan {
    std::string id, name, epg;
    int days = 0;               /* catch-up */
    size_t abs = 0;             /* row in the host's list */
};
struct GridProg {
    long long a = 0, b = 0;
    std::string title, desc;
};
struct GridState {
    bool open = false;
    std::string provider;
    std::vector<GridChan> ch;
    int sel = 0, top = 0;
    long long win = 0;          /* window start */
    long long cursor = 0;       /* the focused programme is the one holding this time */
} g_grid;

/* A folder (or a search) to open as soon as its provider is open: Home's rows
 * and "Go to series". Set by openFromHome(), consumed by update(). */
struct LaunchFolder {
    bool        active = false;
    bool        search = false;
    std::string provider;
    std::string id;           /* the folder id, or the search text */
    std::string title;
};
LaunchFolder g_launchFolder;

/* #101: what the chooser lists - every provider that is set up, plus every one
 * that can be set up from it. With one entry the rail slot goes straight in;
 * with more it shows the chooser, which is also the only place a second
 * provider can get its source (hardware, 2026-09-26: with just IPTV set up the
 * slot went straight to IPTV and Emby could never be reached). */
bool is_pickable(const evo_provider_t* p)
{
    return p && (evo_provider_is_enabled(p->id) || (p->caps & EVO_PROVIDER_CAP_CONFIG));
}

int pickable_count(std::string* only)
{
    int n = 0;
    for (int i = 0; i < evo_provider_count(); ++i) {
        const evo_provider_t* p = evo_provider_at(i);
        if (!is_pickable(p)) continue;
        ++n;
        if (only) *only = p->id;
    }
    return n;
}

/*
 * The resolve callback's state.
 *
 * File-scope rather than a member because the callback is a C function pointer
 * with a void* and the screen outlives the process; carrying the screen pointer
 * would be an invitation to use it after a navigate. What it actually needs is
 * only the title and identity to build a PlaybackSource, all of which is copied
 * here at activation time.
 */
/* The stream picker shows its rows in the generic list document, which holds
 * nine. */
constexpr int kMaxChoices = EVO_RMLUI_LIST_ROWS;

/* Where a row of the picker came from. */
enum ChoiceKind {
    KindListed = 0,     /* the URL the playlist gave                        */
    KindVariant,        /* a quality variant read from its HLS master       */
    KindGuess,          /* an alternative the provider derived (.ts <-> .m3u8) */
    KindSource          /* one of several real sources (a Stremio addon's list) */
};

struct PendingPlay {
    char provider[EVO_PROVIDER_MAX_ID];
    char item_id[EVO_PROVIDER_MAX_ITEM_ID];
    char title[EVO_PROVIDER_MAX_TITLE];
    int  is_live;
    long long resume_sec;   /* the service's resume point, 0 = from the start */
    evo_stream_choice_t choices[kMaxChoices];
    int  kind[kMaxChoices];
    int  choice_count;
    int  active_choice;
    int  ask;           /* the picker is on for this activation              */
    int  picked;        /* the user chose this stream: no automatic fallback */
};
PendingPlay g_pending;

/*
 * Where the picker is in its life. All of it runs on the main thread - the
 * resolver and the variants fetch both deliver from evo_net_poll() - so plain
 * globals are enough; the generation only exists so a reply that lands after the
 * user has backed out is recognised as stale and dropped.
 */
enum class PickStage { Idle, Ready };
PickStage g_pick_stage = PickStage::Idle;
unsigned  g_pick_gen = 0;

/*
 * The quality variants are read while the picker is already on screen: it opens at
 * once with the rows it knows (the URL as listed, and any guess) and the variants
 * are inserted when the read lands. Waiting for the read before showing anything
 * cost the user 8 s of nothing on a host whose first address was dead, and then
 * showed no qualities at all.
 */
enum class VariantsState { None, Reading, Done, Failed };
VariantsState g_variants = VariantsState::None;
int       g_variants_added = 0;         /* rows inserted, waiting to be reflected in the cursor */
bool      g_variants_merged = false;
std::chrono::steady_clock::time_point g_variants_started;
constexpr int kVariantsGiveUpMs = 15000; /* stop saying "reading" after this */

/*
 * Set by on_resolved, cleared by update(). The callback must not reach back
 * into the screen - it can land on a frame after the user has navigated away -
 * so it reports completion through a flag the screen polls instead.
 */
bool g_resolve_finished = false;

static PlaybackSource g_start_src;
static double g_start_resume = 0.0;
static pthread_t g_start_thread;
static std::atomic<bool> g_start_running{false};
static std::atomic<bool> g_start_done{false};
static std::atomic<bool> g_start_success{false};

static void* start_playback_worker(void* arg)
{
    (void)arg;
    auto pb = Application::getInstance().getPlaybackController();
    if (!pb) {
        g_start_success = false;
        g_start_done = true;
        return nullptr;
    }

    bool ok = pb->startPlaybackSource(g_start_src, g_start_resume);
    g_start_success = ok;
    g_start_done = true;
    return nullptr;
}

/* Starts `idx` of the pending list on the playback worker. 0 or a pthread error. */
int launch_choice(int idx)
{
    g_pending.active_choice = idx;
    const evo_stream_choice_t& c = g_pending.choices[idx];
    if (const evo_provider_t* p = evo_provider_find(g_pending.provider))
        if (p->stream_chosen) p->stream_chosen(g_pending.item_id, &c);

    PlaybackSource src;
    src.url      = c.url;
    src.title    = g_pending.title;
    src.provider = g_pending.provider;
    src.item_id  = g_pending.item_id;
    src.art_url  = c.art_url;
    src.year     = c.year;
    src.ep_total = c.ep_total;
    src.overview = c.overview;
    /* Either side may know it is live: the provider's catalog said so, or the
     * resolved choice did (an HLS playlist with no EXT-X-ENDLIST). */
    src.is_live  = (g_pending.is_live || c.is_live) ? true : false;

    /* Read by evo_stream_io_open() on the worker started below. */
    snprintf(evo_stream_headers, sizeof evo_stream_headers, "%s", c.headers);
    evo_stream_max_connections = c.max_connections;
    evo_stream_user_agent[0] = '\0';

    g_start_src = src;
    g_start_resume = src.is_live ? 0.0 : (double)g_pending.resume_sec;
    g_start_done = false;
    g_start_success = false;
    g_start_running = true;

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 2 * 1024 * 1024);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE);
    int rc = pthread_create(&g_start_thread, &attr, start_playback_worker, nullptr);
    pthread_attr_destroy(&attr);

    if (rc != 0) {
        g_start_running = false;
        g_resolve_finished = true;
        evo_bt("provider: pthread_create failed for playback worker rc=%d", rc);
        evo_rmlui_provider_set_tuning(0);
        evo_rmlui_provider_set_loading(0, "");
        evo_rmlui_provider_set_status("Failed to start playback worker thread", 1);
    }
    return rc;
}

bool choice_is_hls(const evo_stream_choice_t& c)
{
    if (!std::strcmp(c.container, "hls")) return true;
    std::string u(c.url);
    std::transform(u.begin(), u.end(), u.begin(), [](unsigned char ch) { return std::tolower(ch); });
    return u.find(".m3u8") != std::string::npos;
}

/*
 * What a source row says about its file, when the provider knows anything:
 * "4K HEVC  -  Dolby Vision  -  24.5 GB  -  45.0 Mbps  -  TRUEHD 7.1" (#116).
 * "" when it knows nothing, and the row falls back to "Source N of M".
 */
std::string source_detail(const evo_stream_choice_t& c)
{
    std::string d;
    auto add = [&d](const std::string& s) {
        if (s.empty()) return;
        if (!d.empty()) d += "  -  ";
        d += s;
    };
    auto upper = [](const char* s) {
        std::string u(s);
        std::transform(u.begin(), u.end(), u.begin(), [](unsigned char ch) { return std::toupper(ch); });
        return u;
    };
    char buf[48];
    std::string video;
    if (c.width >= 3200 || c.height >= 1800)      video = "4K";
    else if (c.width >= 1800 || c.height >= 1000) video = "1080p";
    else if (c.width >= 1200 || c.height >= 700)  video = "720p";
    else if (c.height > 0) { std::snprintf(buf, sizeof buf, "%dp", c.height); video = buf; }
    if (c.video_codec[0]) video += (video.empty() ? "" : " ") + upper(c.video_codec);
    add(video);
    add(c.video_range);
    if (c.size_bytes > 0) {
        const double gb = (double)c.size_bytes / (1024.0 * 1024.0 * 1024.0);
        if (gb >= 1.0) std::snprintf(buf, sizeof buf, "%.1f GB", gb);
        else           std::snprintf(buf, sizeof buf, "%.0f MB", gb * 1024.0);
        add(buf);
    }
    if (c.bitrate_bps > 0) {
        std::snprintf(buf, sizeof buf, "%.1f Mbps", (double)c.bitrate_bps / 1e6);
        add(buf);
    }
    std::string audio = upper(c.audio_codec);
    if (c.audio_channels > 0) {
        const int ch = c.audio_channels;
        if (ch == 1)      std::snprintf(buf, sizeof buf, "Mono");
        else if (ch == 2) std::snprintf(buf, sizeof buf, "Stereo");
        else              std::snprintf(buf, sizeof buf, "%d.1", ch - 1);
        audio += (audio.empty() ? "" : " ") + std::string(buf);
    }
    add(audio);
    return d;
}

/*
 * The master playlist's variants have come back (or the read failed: count 0).
 * The list becomes: the URL as listed, then the variants best first, then the
 * provider's own alternatives, which are guesses.
 */
void on_variants(int count, const evo_stream_choice_t* variants, void* ud)
{
    if ((unsigned)(uintptr_t)ud != g_pick_gen || g_variants != VariantsState::Reading)
        return;                                       /* the user has moved on */

    PendingPlay& pp = g_pending;
    const int before = pp.choice_count;
    evo_stream_choice_t listed = pp.choices[0];
    evo_stream_choice_t guesses[kMaxChoices];
    int ng = 0;
    for (int i = 1; i < pp.choice_count && ng < kMaxChoices; ++i)
        guesses[ng++] = pp.choices[i];

    int n = 0;
    pp.choices[n] = listed;  pp.kind[n++] = KindListed;
    for (int i = 0; i < count && n < kMaxChoices; ++i) {
        pp.choices[n] = variants[i];
        pp.kind[n++] = KindVariant;
    }
    for (int i = 0; i < ng && n < kMaxChoices; ++i) {
        pp.choices[n] = guesses[i];
        pp.kind[n++] = KindGuess;
    }
    pp.choice_count = n;
    evo_bt("provider: stream picker: %d variant(s) read for %s, %d choice(s) in all",
           count, pp.title, n);

    /* update() reflects it on screen: the picker is already open, and the rows
     * after the listed one have just moved down. */
    g_variants = (count > 0) ? VariantsState::Done : VariantsState::Failed;
    g_variants_added = n - before;
    g_variants_merged = true;
}

void on_resolved(int ok, const evo_stream_choice_t* choices, int count, void* ud)
{
    PendingPlay* pp = (PendingPlay*)ud;

    if (!ok || count <= 0 || !choices) {
        g_resolve_finished = true;
        evo_bt("provider: resolve failed for %s/%s", pp->provider, pp->item_id);
        char why[256];
        const bool have = evo_failure_reason(why, sizeof why) != 0;
        toast("STREAM", have ? why : (pp->is_live ? "Failed to resolve channel stream"
                                                  : "Failed to resolve stream"));
        evo_rmlui_provider_set_tuning(0);
        evo_rmlui_provider_set_loading(0, "");
        evo_rmlui_provider_set_status(have ? why : (pp->is_live ? "Failed to resolve stream for channel"
                                                               : "Failed to resolve stream"), 1);
        return;
    }

    /*
     * Best first, by contract. Anything after the first is an alternative the
     * provider derived: with the picker off it is only ever tried automatically
     * if the first fails to open; with it on it is listed, and marked a guess.
     */
    pp->choice_count = std::min(count, kMaxChoices);
    const evo_provider_t* rp = evo_provider_find(pp->provider);
    const bool sources = rp && (rp->caps & EVO_PROVIDER_CAP_PICK);
    for (int i = 0; i < pp->choice_count; ++i) {
        pp->choices[i] = choices[i];
        pp->kind[i] = sources ? KindSource : (i == 0) ? KindListed : KindGuess;
    }
    pp->active_choice = 0;

    if (pp->ask && sources) {
        /* Every row is a real source the addon listed: no guesses, and no HLS
         * quality read - its rows would be inserted as if after a playlist URL. */
        g_variants = VariantsState::None;
        if (pp->choice_count > 1) {
            g_pick_stage = PickStage::Ready;
            return;
        }
    } else if (pp->ask) {
        /* An HLS master lists the same channel at several qualities. The list
         * opens now with what is known and the qualities are added when the read
         * lands; if the read cannot even be queued the list has fewer rows. */
        g_variants = VariantsState::None;
        if (choice_is_hls(pp->choices[0])) {
            g_variants_started = std::chrono::steady_clock::now();
            g_variants = (evo_hls_variants_fetch(pp->choices[0].url, on_variants,
                                                 (void*)(uintptr_t)g_pick_gen) == 0)
                       ? VariantsState::Reading : VariantsState::Failed;
        }
        if (pp->choice_count > 1 || g_variants == VariantsState::Reading) {
            g_pick_stage = PickStage::Ready;              /* update() opens the picker */
            return;
        }
        /* One stream, nothing behind it, nothing being read: nothing to choose. */
    }

    launch_choice(0);
}

} // namespace

ProviderHostScreen::ProviderHostScreen()
    : StatefulScreen("ProviderHostScreen") {
}

void ProviderHostScreen::setPendingProvider(const std::string& id)
{
    g_pendingProvider = id;
}

/* ---- Resume without the portal screen ---------------------------------- */

static bool g_headless = false;

ProviderHostScreen::HeadlessBegin
ProviderHostScreen::beginHeadlessResume(const char* key, const char* title,
                                        double resumeSec, std::string& error)
{
    error.clear();
    if (!key || std::strncmp(key, "evo://", 6) != 0) return HeadlessBegin::NotProvider;

    const char* rest = key + 6;
    const char* slash = std::strchr(rest, '/');
    if (!slash || slash == rest || !slash[1]) {
        error = "This entry is damaged - remove it and play it again";
        return HeadlessBegin::Failed;
    }
    const std::string provider(rest, static_cast<size_t>(slash - rest));
    const std::string itemId(slash + 1);

    const evo_provider_t* p = evo_provider_find(provider.c_str());
    if (!p) {
        error = "That source is no longer available";
        return HeadlessBegin::Failed;
    }
    /* A server that has to be signed in to again is the portal's job. */
    if (p->needs_sign_in && p->sign_in && p->needs_sign_in()) return HeadlessBegin::NeedsPortal;

    if (g_headless || g_start_running) {
        error = "Still opening the last item - one moment";
        return HeadlessBegin::Failed;
    }

    std::memset(&g_pending, 0, sizeof g_pending);
    std::snprintf(g_pending.provider, sizeof g_pending.provider, "%s", provider.c_str());
    std::snprintf(g_pending.item_id,  sizeof g_pending.item_id,  "%s", itemId.c_str());
    std::snprintf(g_pending.title,    sizeof g_pending.title,    "%s",
                  (title && title[0]) ? title : itemId.c_str());
    g_pending.is_live = 0;
    g_pending.resume_sec = resumeSec > 1.0 ? static_cast<long long>(resumeSec) : 0;
    g_pending.ask = 0;                 /* Resume plays the best source; no picker */
    g_pending.picked = 0;
    g_pick_stage = PickStage::Idle;
    ++g_pick_gen;
    g_variants = VariantsState::None;
    g_variants_merged = false;
    g_resolve_finished = false;

    g_headless = true;
    if (evo_provider_resolve_chain(g_pending.provider, g_pending.item_id, on_resolved, &g_pending) != 0) {
        g_headless = false;
        char why[256];
        error = evo_failure_reason(why, sizeof why) ? why : "That item could not be played";
        return HeadlessBegin::Failed;
    }
    return HeadlessBegin::Started;
}

ProviderHostScreen::HeadlessPoll ProviderHostScreen::pollHeadlessResume(std::string& error)
{
    error.clear();
    if (!g_headless) return HeadlessPoll::Idle;

    if (g_start_running && g_start_done) {
        pthread_join(g_start_thread, nullptr);
        g_start_running = false;
        g_resolve_finished = false;
        if (g_start_success) {
            g_headless = false;
            return HeadlessPoll::Ready;
        }
        /* The best source would not open: try the next one the provider listed. */
        if (g_pending.active_choice + 1 < g_pending.choice_count &&
            launch_choice(g_pending.active_choice + 1) == 0)
            return HeadlessPoll::Working;
        g_headless = false;
        error = "The stream would not open";
        return HeadlessPoll::Failed;
    }
    if (!g_start_running && g_resolve_finished) {
        /* on_resolved has already put its reason on screen. */
        g_resolve_finished = false;
        g_headless = false;
        return HeadlessPoll::Failed;
    }
    return HeadlessPoll::Working;
}

bool ProviderHostScreen::resumeFromRecent(const char* key, const char* title, double resumeSec)
{
    if (!key || std::strncmp(key, "evo://", 6) != 0) return false;

    const char* rest = key + 6;
    const char* slash = std::strchr(rest, '/');
    if (!slash || slash == rest || !slash[1]) {
        toast("CONTINUE WATCHING", "This entry is damaged - remove it and play it again");
        return true;
    }
    const std::string provider(rest, static_cast<size_t>(slash - rest));
    const std::string itemId(slash + 1);

    const evo_provider_t* p = evo_provider_find(provider.c_str());
    if (!p) {
        toast("CONTINUE WATCHING", "That source is no longer available");
        return true;
    }

    g_launchResume.active = true;
    g_launchResume.provider = provider;
    g_launchResume.item_id = itemId;
    g_launchResume.title = (title && title[0]) ? title : itemId;
    g_launchResume.resume_sec = resumeSec > 1.0 ? static_cast<long long>(resumeSec) : 0;

    g_pendingProvider = provider;
    if (auto sm = Application::getInstance().getScreenManager())
        sm->navigateTo(ScreenId::EmbyBrowse);
    return true;
}

bool ProviderHostScreen::openFromHome(const char* key, const char* title, bool search)
{
    if (!key || std::strncmp(key, "evo://", 6) != 0) return false;
    const char* rest = key + 6;
    const char* slash = std::strchr(rest, '/');
    if (!slash || slash == rest || !slash[1]) return false;
    const std::string provider(rest, static_cast<size_t>(slash - rest));
    if (!evo_provider_find(provider.c_str())) {
        toast("HOME", "That source is no longer available");
        return true;
    }
    g_launchResume.active = false;
    g_launchFolder.active = true;
    g_launchFolder.search = search;
    g_launchFolder.provider = provider;
    g_launchFolder.id = search ? std::string(title ? title : "") : std::string(slash + 1);
    g_launchFolder.title = (title && title[0]) ? title : "";
    g_pendingProvider = provider;
    if (auto sm = Application::getInstance().getScreenManager())
        sm->navigateTo(ScreenId::EmbyBrowse);
    return true;
}

namespace { int parentalGate(const char* provider, const char* id, const char* title); }

void ProviderHostScreen::onEnter()
{
    StatefulScreen::onEnter();
    evo_rmlui_provider_set_folder_gate(&parentalGate);   /* locked categories ask for the PIN */
    m_resolving = false;
    m_tunePending = false;
    m_tuneFrames = 0;
    evo_rmlui_provider_set_tuning(0);
    evo_rmlui_provider_set_loading(0, "");

    /* #101: back from a stream a web page handed over. The page reopens by
     * itself (evo_webui.c); this screen just keeps hosting it. */
    if (m_web && evo_webui_session_active()) {
        m_navigatingToPlayer = false;
        return;
    }

    /* Used once: Home / Continue Watching asks for a provider for this visit
     * only. It used to stay set, so Providers in the side menu kept opening
     * the Stalker portal instead of the list of providers. */
    std::string want = g_pendingProvider;
    g_pendingProvider.clear();
    if (want.empty()) {
        /* Returning from Player playback: keep the active session intact so
         * the user lands back in the exact folder and on the exact channel
         * card they left. */
        if (m_opened && m_navigatingToPlayer) {
            m_navigatingToPlayer = false;
            evo_bt("prov_screen: returning from playback, keeping provider '%s' open",
                   m_providerId.c_str());
            /* A service that tracks progress has just been told where playback
             * stopped: re-read the level so the card shows it (same cursor). */
            const evo_provider_t* rp = evo_provider_find(m_providerId.c_str());
            if (rp && (rp->caps & EVO_PROVIDER_CAP_PROGRESS) && !(rp->caps & EVO_PROVIDER_CAP_LIVE))
                evo_rmlui_provider_reload();
            return;
        }
        /* One provider to offer: the rail slot means that provider, as it
         * always has. Several: the chooser. */
        std::string only;
        if (pickable_count(&only) != 1) {
            enterPicker();
            return;
        }
        want = only;
    }
    openProvider(want);
}

void ProviderHostScreen::openProvider(const std::string& want)
{
    m_picking = false;
    const evo_provider_t* wp = evo_provider_find(want.c_str());
    if (wp && (wp->caps & EVO_PROVIDER_CAP_WEBUI) && !isNativeWeb(wp)) {
        if (m_opened) {
            evo_rmlui_provider_close();
            m_opened = false;
        }
        m_providerId = want;
        openWebProvider();
        return;
    }
    m_web = false;

    /* A media server with no session yet: sign in first, then come back. */
    if (wp && wp->needs_sign_in && wp->needs_sign_in()) {
        if (m_opened) {
            evo_rmlui_provider_close();
            m_opened = false;
        }
        m_providerId = want;
        beginSignIn();
        return;
    }

    /* Returning from Player playback into the same provider: keep it. */
    if (m_opened && m_providerId == want) {
        m_navigatingToPlayer = false;
        evo_bt("prov_screen: returning from playback, keeping provider '%s' open", m_providerId.c_str());
        return;
    }

    /* If switching to a different provider, close previous host */
    if (m_opened) {
        evo_rmlui_provider_close();
        m_opened = false;
    }

    m_navigatingToPlayer = false;
    m_providerId = want;
    if (m_lastTypedUrl.empty()) {
        const evo_provider_t* p = evo_provider_find(want.c_str());
        if (p && p->get_source) {
            const char* s = p->get_source();
            if (s && (std::strncmp(s, "http://", 7) == 0 || std::strncmp(s, "https://", 8) == 0)) {
                m_lastTypedUrl = s;
            }
        }
    }
    evo_bt("prov_screen: onEnter calling evo_rmlui_provider_open('%s')", m_providerId.c_str());
    m_opened = evo_rmlui_provider_open(m_providerId.c_str(),
                                        DisplayWidth, DisplayHeight) != 0;
    evo_bt("prov_screen: onEnter evo_rmlui_provider_open returned opened=%d", m_opened ? 1 : 0);
    if (m_opened) {
        /* First run: nothing to browse yet, so land on the provider's setup
         * page (IPTV: type an M3U URL or pick one from USB). */
        const evo_provider_t* p = evo_provider_find(m_providerId.c_str());
        if (p && p->is_configured && !p->is_configured() && (p->caps & EVO_PROVIDER_CAP_CONFIG))
            evo_rmlui_provider_show_setup();
    }
    if (!m_opened) {
        evo_bt("provider: could not open host for '%s'", m_providerId.c_str());
        toast("PROVIDERS", "That provider is not available");
        if (auto sm = Application::getInstance().getScreenManager())
            sm->navigateTo(ScreenId::MainMenu);
    }
}

/* ------------------------------------------------------------------------- */
/* #101: web-UI providers                                                    */
/* ------------------------------------------------------------------------- */

void ProviderHostScreen::openWebProvider()
{
    const evo_provider_t* p = evo_provider_find(m_providerId.c_str());
    if (!p || !p->web_ui_url) return;
    const char* url = p->web_ui_url();
    if (!url || !url[0]) {
        /* Not set up yet: ask for the address, and open once it is in. The
         * chooser sits behind the prompt, so cancelling it is not a blank
         * screen with no way out. */
        std::string id = m_providerId;
        enterPicker();
        m_providerId = id;
        openSourceEditor();
        return;
    }
    /* A server behind a reverse proxy at a path ("https://host/jellyfin"):
     * the proxy upstream is scheme://host[:port], and the page lives under
     * the path. */
    std::string upstream = url, page = p->web_ui_path ? p->web_ui_path : "/";
    {
        size_t s = upstream.find("://");
        size_t slash = upstream.find('/', s == std::string::npos ? 0 : s + 3);
        if (slash != std::string::npos) {
            std::string path = upstream.substr(slash);
            while (!path.empty() && path.back() == '/') path.pop_back();
            upstream.resize(slash);
            page = path + page;
        }
    }
    int rc = evo_webui_open_ex(upstream.c_str(), page.c_str(), p->web_ui_hook);
    evo_bt("prov_screen: web UI '%s' -> %s rc=%d", p->id, url, rc);
    if (rc < 0) {
        toast(p->name, "Set the server as http(s)://<host>:<port>");
        enterPicker();
        return;
    }
    m_web = true;
    m_webSeen = false;
}

/* ------------------------------------------------------------------------- */
/* #101: the chooser                                                         */
/* ------------------------------------------------------------------------- */

void ProviderHostScreen::enterPicker()
{
    if (m_opened) {
        evo_rmlui_provider_close();
        m_opened = false;
    }
    m_web = false;
    m_webSeen = false;
    m_menu.clear();
    m_pickIds.clear();
    m_pickDetail.clear();

    /* Every provider that is set up, plus every one that can be set up from
     * here - so the chooser is also where a new provider gets its source. */
    for (int i = 0; i < evo_provider_count(); ++i) {
        const evo_provider_t* p = evo_provider_at(i);
        if (!is_pickable(p)) continue;

        std::string detail;
        if (!p->is_configured()) {
            detail = (p->caps & EVO_PROVIDER_CAP_WEBUI)
                   ? "Not set up - press X to enter the server address"
                   : p->ui_embedded ? "No addons yet - press X to add one"
                   : "Not set up - press X to add a playlist";
        } else if (isNativeWeb(p)) {
            const char* src = p->get_source ? p->get_source() : "";
            detail = std::string(src ? src : "") +
                     ((p->needs_sign_in && p->needs_sign_in()) ? " - press X to sign in" : "");
        } else if (p->caps & EVO_PROVIDER_CAP_WEBUI) {
            /* An address carries no secret; a playlist URL can (Xtream user and
             * password), so only the web kind shows its source. */
            const char* src = p->get_source ? p->get_source() : "";
            detail = std::string("Web UI - ") + (src ? src : "");
        } else {
            detail = "Ready";
        }
        if (p->sign_out && p->is_signed_in && p->is_signed_in())
            detail += "  \u00b7  Signed in";
        m_pickIds.push_back(p->id);
        m_pickDetail.push_back(detail);
    }

    if (m_pickIds.empty()) {
        /* No provider can even be configured. Not an error - it is the state a
         * build with every provider compiled out is in. */
        toast("PROVIDERS", "No provider is set up yet");
        if (auto sm = Application::getInstance().getScreenManager())
            sm->navigateTo(ScreenId::Settings);
        return;
    }
    if (m_pickIndex < 0 || m_pickIndex >= (int)m_pickIds.size())
        m_pickIndex = 0;
    m_picking = true;
}

void ProviderHostScreen::choosePicked(bool editSource)
{
    if (m_pickIndex < 0 || m_pickIndex >= (int)m_pickIds.size()) return;
    const std::string id = m_pickIds[m_pickIndex];
    const evo_provider_t* p = evo_provider_find(id.c_str());
    if (!p) return;

    m_providerId = id;
    bool configured = p->is_configured() != 0;
    if (editSource || !configured) {
        if (!(p->caps & EVO_PROVIDER_CAP_CONFIG)) {
            toast(p->name, "Nothing to set up here");
            return;
        }
        evo_feedback(EVO_FB_OPEN);
        if ((p->caps & EVO_PROVIDER_CAP_WEBUI) || p->ui_embedded) {
            openSourceEditor();     /* the address; opens the provider once set */
            return;
        }
        /* A bundle provider has its own setup page (IPTV: type a URL, or pick
         * an .m3u from USB) - open the provider and go straight to it rather
         * than to a bare keyboard. */
        openProvider(id);
        if (m_opened)
            evo_rmlui_provider_show_setup();
        return;
    }
    /* Configured but switched off (a flag saved before it had a source):
     * choosing it is the user asking for it. */
    if (!evo_provider_is_enabled(p->id))
        evo_provider_set_enabled(p->id, 1);
    evo_feedback(EVO_FB_CONFIRM);
    openProvider(id);
}

void ProviderHostScreen::renderPicker(uint32_t* framebuffer, int width, int height)
{
    evo_rmlui_list_params_t params;
    std::memset(&params, 0, sizeof(params));

    bool railFocused = false;
    if (auto sm = Application::getInstance().getScreenManager())
        railFocused = sm->isRailFocused();

    params.section = EVO_SECTION_EMBY;
    params.rail_focused = railFocused ? 1 : 0;

    if (m_signIn == SignIn::QcStart || m_signIn == SignIn::QcWait ||
        m_signIn == SignIn::QcPolling) {
        renderQuickConnect(framebuffer, width, height);
        return;
    }

    if (m_web) {
        /* Behind the browser, which covers everything right of the rail. */
        const evo_provider_t* p = evo_provider_find(m_providerId.c_str());
        params.title = p ? p->name : "PROVIDER";
        params.subtitle = "Opening the web UI";
        params.is_empty = 1;
        params.empty_title = "Opening...";
        params.empty_hint = "The page opens beside the menu. Close it to come back here.";
        params.empty_icon = "../icons/icon_emby.png";
        evo_rmlui_update_list(&params);
        evo_rmlui_render_list(framebuffer, width, height);
        return;
    }

    params.title = "PROVIDERS";
    params.subtitle = "Choose where to watch from";
    int total = (int)m_pickIds.size();
    params.total_count = total;
    params.cursor_index = total ? m_pickIndex : -1;
    int rows = std::min(EVO_RMLUI_LIST_ROWS, total);
    params.row_count = rows;
    for (int i = 0; i < rows; ++i) {
        const evo_provider_t* p = evo_provider_find(m_pickIds[i].c_str());
        bool web = p && (p->caps & EVO_PROVIDER_CAP_WEBUI) && !isNativeWeb(p);
        bool server = p && isNativeWeb(p);
        params.rows[i].title = p ? p->name : m_pickIds[i].c_str();
        params.rows[i].detail = m_pickDetail[i].c_str();
        params.rows[i].icon_path = (web || server) ? "../icons/icon_emby.png" : "../icons/icon_folder.png";
        params.rows[i].badge = web ? "WEB" : server ? "SERVER"
                             : (p && (p->caps & EVO_PROVIDER_CAP_LIVE)) ? "LIVE" : "";
        params.rows[i].progress = -1;
        params.rows[i].has_chevron = 1;
        params.rows[i].is_focused = (!railFocused && i == m_pickIndex);
    }

    const evo_provider_t* fp = (m_pickIndex >= 0 && m_pickIndex < total)
                             ? evo_provider_find(m_pickIds[m_pickIndex].c_str()) : nullptr;
    params.hint_count = 0;
    params.hints[params.hint_count].glyph_path = "../icons/btn_cross.png";
    params.hints[params.hint_count++].label = "OPEN";
    /* Square still edits the address / playlist directly; the OPTIONS menu
     * lists that and everything else, so it is the one announced. */
    params.hints[params.hint_count].glyph_path = "../icons/icon_settings.png";
    params.hints[params.hint_count++].label = "OPTIONS";
    if (fp && isNativeWeb(fp) && fp->is_configured()) {
        params.hints[params.hint_count].glyph_path = "../icons/btn_triangle.png";
        params.hints[params.hint_count++].label = "WEB VERSION";
    }
    params.hints[params.hint_count].glyph_path = "../icons/btn_circle.png";
    params.hints[params.hint_count++].label = "BACK";

    if (!m_menu.empty() && fp) {
        params.menu_count = std::min((int)m_menu.size(), EVO_RMLUI_LIST_MENU_ROWS);
        params.menu_focus = m_menuIndex;
        params.menu_eyebrow = "OPTIONS";
        params.menu_title = fp->name;
        params.menu_sub = m_pickDetail[m_pickIndex].c_str();
        params.menu_icon = (fp->caps & EVO_PROVIDER_CAP_WEBUI) ? "../icons/icon_emby.png"
                                                               : "../icons/icon_folder.png";
        for (int i = 0; i < params.menu_count; ++i) {
            params.menu[i].label = m_menu[i].label.c_str();
            params.menu[i].desc = m_menu[i].desc.c_str();
            params.menu[i].icon_path = m_menu[i].icon.c_str();
            params.menu[i].danger = m_menu[i].danger ? 1 : 0;
        }
    }

    evo_rmlui_update_list(&params);
    evo_rmlui_render_list(framebuffer, width, height);
}

/* ------------------------------------------------------------------------- */
/* The chooser's OPTIONS menu                                                */
/* ------------------------------------------------------------------------- */

void ProviderHostScreen::openPickerMenu()
{
    if (m_pickIndex < 0 || m_pickIndex >= (int)m_pickIds.size()) return;
    const std::string id = m_pickIds[m_pickIndex];
    const evo_provider_t* p = evo_provider_find(id.c_str());
    if (!p) return;

    const bool web = (p->caps & EVO_PROVIDER_CAP_WEBUI) != 0;
    const bool server = isNativeWeb(p);
    const bool configured = p->is_configured() != 0;
    const bool signedIn = p->is_signed_in && p->is_signed_in();
    const char* src = p->get_source ? p->get_source() : "";

    m_menu.clear();
    auto add = [this](const char* mid, const std::string& label, const std::string& desc,
                      const char* icon, bool danger) {
        MenuItem m;
        m.id = mid; m.label = label; m.desc = desc; m.icon = icon; m.danger = danger;
        m_menu.push_back(m);
    };

    if (configured)
        add("open", "Open", server ? "Browse this server's libraries" : "Open this provider",
            "../icons/icon_resume.png", false);
    if (p->caps & EVO_PROVIDER_CAP_CONFIG) {
        if (web) {
            add("address", configured ? "Change server" : "Set server address",
                (src && src[0]) ? std::string("Address and port - now ") + src
                                : std::string("Type http(s)://host:port"),
                "../icons/icon_keyboard.png", false);
            if (id == "emby" || id == "jellyfin")
                add("discover", "Find server on network",
                    std::string("Look for ") + p->name + " on your home network",
                    "../icons/icon_activity.png", false);
        } else if (p->ui_embedded) {
            add("address", "Add Stremio addon", "Paste an addon's manifest URL (https://.../manifest.json)",
                "../icons/icon_keyboard.png", false);
        } else {
            add("address", configured ? "Change playlist" : "Add playlist",
                "Type a URL or pick an .m3u from USB", "../icons/icon_tv.png", false);
        }
    }
    if (configured && p->sign_in)
        add("signin", signedIn ? "Sign in as another user" : "Sign in",
            signedIn ? "Switch account on this server" : "User name and password, or Quick Connect",
            "../icons/icon_type.png", false);
    if (configured && server)
        add("webui", "Web version", "Open the server's own site in the browser",
            "../icons/icon_emby.png", false);
    if (signedIn && p->sign_out)
        add("signout", "Sign out", "Forget the saved session on this console",
            "../icons/icon_power.png", true);

    if (m_menu.empty()) {
        evo_feedback(EVO_FB_BOUNDARY);
        return;
    }
    if ((int)m_menu.size() > EVO_RMLUI_LIST_MENU_ROWS)
        m_menu.resize(EVO_RMLUI_LIST_MENU_ROWS);
    m_menuIndex = 0;
    evo_feedback(EVO_FB_OPEN);
    evo_bt("prov_screen: options menu for '%s' (%d rows)", p->id, (int)m_menu.size());
}

bool ProviderHostScreen::pickerMenuInput(uint32_t pressed)
{
    const int n = (int)m_menu.size();
    if (pressed & PadButtons::Up) {
        if (m_menuIndex > 0) { --m_menuIndex; evo_feedback(EVO_FB_MOVE); }
        else evo_feedback(EVO_FB_BOUNDARY);
    } else if (pressed & PadButtons::Down) {
        if (m_menuIndex + 1 < n) { ++m_menuIndex; evo_feedback(EVO_FB_MOVE); }
        else evo_feedback(EVO_FB_BOUNDARY);
    } else if (pressed & PadButtons::Cross) {
        std::string id = (m_menuIndex >= 0 && m_menuIndex < n) ? m_menu[m_menuIndex].id : "";
        m_menu.clear();
        runPickerMenu(id);
    } else if (pressed & (PadButtons::Circle | PadButtons::Options)) {
        evo_feedback(EVO_FB_CANCEL);
        m_menu.clear();
    }
    return true;        /* the menu has the pad while it is open */
}

void ProviderHostScreen::runPickerMenu(const std::string& action)
{
    if (m_pickIndex < 0 || m_pickIndex >= (int)m_pickIds.size()) return;
    const std::string id = m_pickIds[m_pickIndex];
    const evo_provider_t* p = evo_provider_find(id.c_str());
    if (!p) return;
    evo_bt("prov_screen: options '%s' on '%s'", action.c_str(), id.c_str());

    if (action == "open") {
        choosePicked(false);
    } else if (action == "address") {
        if (p->caps & EVO_PROVIDER_CAP_WEBUI) {
            /* Straight to the keyboard with the saved address, so a new host
             * or port is a few presses - no LAN search in the way. */
            m_providerId = id;
            const char* cur = p->get_source ? p->get_source() : "";
            openSourceKeyboard((cur && cur[0]) ? cur : "http://");
        } else {
            choosePicked(true);
        }
    } else if (action == "discover") {
        m_providerId = id;
        discoverServer();
    } else if (action == "signin") {
        /* The saved session stays until the new sign-in succeeds, so backing
         * out of the keyboard costs nothing. */
        evo_feedback(EVO_FB_OPEN);
        m_providerId = id;
        beginSignIn();
    } else if (action == "webui") {
        evo_feedback(EVO_FB_OPEN);
        m_providerId = id;
        m_picking = false;
        openWebProvider();
        if (!m_web) enterPicker();
    } else if (action == "signout") {
        p->sign_out();
        evo_feedback(EVO_FB_CONFIRM);
        evo_bt("prov_screen: signed out of '%s'", p->id);
        toast(p->name, "Signed out");
        int keep = m_pickIndex;
        enterPicker();
        m_pickIndex = keep;
    }
}

void ProviderHostScreen::discoverServer()
{
    const evo_provider_t* p = evo_provider_find(m_providerId.c_str());
    if (!p) return;
    const char* product = m_providerId == "emby"     ? "EmbyServer"
                        : m_providerId == "jellyfin" ? "JellyfinServer"
                        : nullptr;
    if (!product) {
        openSourceKeyboard("http://");
        return;
    }
    evo_net_discover_start(product);
    m_discovering = true;
    evo_feedback(EVO_FB_OPEN);
    char msg[96];
    std::snprintf(msg, sizeof msg, "Looking for %s on your network...", p->name);
    toast(p->name, msg);
}

void ProviderHostScreen::enterStreamPicker()
{
    g_pick_stage = PickStage::Idle;
    m_streamPick = true;
    m_streamIndex = 0;
    m_resolving = false;
    m_tunePending = false;
    evo_rmlui_provider_set_tuning(0);
    evo_rmlui_provider_set_loading(0, "");
    evo_feedback(EVO_FB_OPEN);
}

void ProviderHostScreen::cancelStreamPicker()
{
    m_streamPick = false;
    g_pick_stage = PickStage::Idle;
    ++g_pick_gen;
    g_variants = VariantsState::None;
    g_variants_merged = false;
    m_resolving = false;
}

void ProviderHostScreen::chooseStream(int index)
{
    if (index < 0 || index >= g_pending.choice_count) return;
    if (g_start_running) return;

    evo_feedback(EVO_FB_CONFIRM);
    g_pending.picked = 1;
    m_streamPick = false;
    m_resolving = true;

    /* A quality read still in flight is no longer wanted: drop its reply. */
    ++g_pick_gen;
    if (g_variants == VariantsState::Reading) g_variants = VariantsState::None;

    char msg[128];
    std::snprintf(msg, sizeof(msg), "Opening %s...", g_pending.choices[index].label);
    /* A media server's versions are not live TV (#116): name the provider. */
    const evo_provider_t* tp = evo_provider_find(g_pending.provider);
    const bool live = g_pending.is_live || (tp && (tp->caps & EVO_PROVIDER_CAP_LIVE));
    toast(live ? "LIVE TV" : (tp ? tp->name : "PROVIDER"), msg);
    evo_rmlui_provider_set_tuning(1);
    evo_rmlui_provider_set_loading(1, msg);
    evo_bt("prov_screen: stream %d chosen for %s: %s", index, g_pending.title,
           g_pending.choices[index].url);

    if (launch_choice(index) != 0) {
        m_resolving = false;
        m_streamPick = true;                 /* it would not start: keep the list */
    }
}

void ProviderHostScreen::renderStreamPicker(uint32_t* framebuffer, int width, int height)
{
    evo_rmlui_list_params_t params;
    std::memset(&params, 0, sizeof(params));

    params.section = EVO_SECTION_EMBY;
    params.rail_focused = 0;
    params.title = "CHOOSE A STREAM";

    /* Say what the list is waiting for, or why it has no qualities: silence here
     * looks like the feature does not exist. */
    static char subtitle[EVO_PROVIDER_MAX_TITLE + 64];
    const char* note = (g_variants == VariantsState::Reading) ? "reading the available qualities..."
                     : (g_variants == VariantsState::Failed)  ? "qualities unavailable - the server did not answer"
                     : nullptr;
    if (note)
        std::snprintf(subtitle, sizeof subtitle, "%s  -  %s", g_pending.title, note);
    else
        std::snprintf(subtitle, sizeof subtitle, "%s", g_pending.title);
    params.subtitle = subtitle;

    static char detail[kMaxChoices][192];
    const int total = g_pending.choice_count;
    params.total_count = total;
    params.cursor_index = total ? m_streamIndex : -1;
    const int rows = std::min(EVO_RMLUI_LIST_ROWS, total);
    params.row_count = rows;

    for (int i = 0; i < rows; ++i) {
        const evo_stream_choice_t& c = g_pending.choices[i];
        const int kind = g_pending.kind[i];

        if (kind == KindSource) {
            const std::string d = source_detail(c);
            if (!d.empty())
                std::snprintf(detail[i], sizeof detail[i], "%s", d.c_str());
            else
                std::snprintf(detail[i], sizeof detail[i], "Source %d of %d%s", i + 1, total,
                              choice_is_hls(c) ? "  -  HLS" : "");
        } else if (kind == KindListed) {
            std::snprintf(detail[i], sizeof detail[i], "%s",
                          choice_is_hls(c) ? "As the playlist lists it - Kijkr picks the quality"
                                           : "As the playlist lists it");
        } else if (kind == KindGuess) {
            std::snprintf(detail[i], sizeof detail[i], "%s",
                          "The same address with a different extension - a guess");
        } else {
            /* a quality variant: what the master says about it */
            std::string d;
            auto add = [&d](const std::string& s) {
                if (s.empty()) return;
                if (!d.empty()) d += "  -  ";
                d += s;
            };
            char buf[48];
            if (c.width > 0 && c.height > 0) {
                std::snprintf(buf, sizeof buf, "%dx%d", c.width, c.height);
                add(buf);
            }
            auto upper = [](const char* s) {
                std::string u(s);
                std::transform(u.begin(), u.end(), u.begin(), [](unsigned char ch) { return std::toupper(ch); });
                return u;
            };
            add(upper(c.video_codec));
            add(upper(c.audio_codec));
            if (c.bitrate_bps > 0) {
                std::snprintf(buf, sizeof buf, "%.1f Mbps", (double)c.bitrate_bps / 1e6);
                add(buf);
            }
            std::snprintf(detail[i], sizeof detail[i], "%s", d.empty() ? "Quality variant" : d.c_str());
        }

        params.rows[i].title = c.label[0] ? c.label : "Stream";
        params.rows[i].detail = detail[i];
        params.rows[i].icon_path = "../icons/icon_tv.png";
        params.rows[i].badge = kind == KindSource ? "SOURCE" : kind == KindListed ? "LISTED"
                             : kind == KindGuess ? "GUESS" : "HLS";
        params.rows[i].progress = -1;
        params.rows[i].has_chevron = 1;
        params.rows[i].is_focused = (i == m_streamIndex);
    }

    params.hint_count = 2;
    params.hints[0].glyph_path = "../icons/btn_cross.png";
    params.hints[0].label = "PLAY";
    params.hints[1].glyph_path = "../icons/btn_circle.png";
    params.hints[1].label = "BACK";

    evo_rmlui_update_list(&params);
    evo_rmlui_render_list(framebuffer, width, height);
}

void ProviderHostScreen::onExit()
{
    g_launchResume.active = false;   /* never fire a stale resume on the next visit */
    g_grid.open = false;
    g_launchFolder.active = false;
    if (g_start_running) {
        pthread_join(g_start_thread, nullptr);
        g_start_running = false;
    }
    m_tunePending = false;
    m_tuneFrames = 0;
    evo_rmlui_provider_set_tuning(0);
    evo_rmlui_provider_set_loading(0, "");
    if (m_opened && !m_navigatingToPlayer) {
        evo_rmlui_provider_close();
        m_opened = false;
    }
    m_resolving = false;
    m_picking = false;
    m_streamPick = false;
    g_pick_stage = PickStage::Idle;
    ++g_pick_gen;
    g_variants = VariantsState::None;
    g_variants_merged = false;
    if (m_web && !evo_webui_session_active()) {
        m_web = false;
        m_webSeen = false;
    }
    StatefulScreen::onExit();
}


/* ------------------------------------------------------------------------- */
/* OPTIONS: the action menu for the item under the cursor (Stalker)          */
/* ------------------------------------------------------------------------- */

namespace {
enum ActionCode {
    ActPlay, ActPlayStart, ActWatched, ActFavorite, ActOpen, ActHide, ActMoveUp,
    ActMoveDown, ActMoveTo, ActSort, ActSearch, ActRestoreHidden, ActPortalSettings,
    ActLock, ActUnlock, ActRelock, ActGuide,
    ActSearchNew, ActSearchClear, ActSearchForget,
    ActChanHide, ActChanUp, ActChanDown, ActChanMoveTo, ActChanShowHidden, ActChanResetOrder,
    ActRemoveSource, ActPlaylistMenu, ActGrid,
    ActHistory = 1000                    /* + the entry: a search from the history */
};

/* "0|vm:123" -> "vm:123" */
const char* inner_id(const char* id)
{
    const char* bar = id ? std::strchr(id, '|') : nullptr;
    return bar ? bar + 1 : (id ? id : "");
}

/* A live channel: Stalker's "ch:", Xtream's "live_stream:", a playlist's "c:" / "n:". */
bool is_channel_id(const char* inner)
{
    return std::strncmp(inner, "ch:", 3) == 0 || std::strncmp(inner, "live_stream:", 12) == 0 ||
           std::strncmp(inner, "c:", 2) == 0 || std::strncmp(inner, "n:", 2) == 0;
}

/* The focused row's channel name as the arrangement and the favourites keep
 * it, "" when the focus is not on a channel of a list. */
std::string focused_chan_key()
{
    const EvoProviderRow* r = EvoRmlProviderHost::Instance().GetFocusedRow();
    return r ? std::string(r->chan_key.c_str()) : std::string();
}

/* The favourites key of the focused item: "evo://<provider>/<id>". A playlist
 * channel is kept by its name ("n:<name>"): its number changes whenever the
 * playlist does. */
std::string fav_key(const std::string& provider, const char* id)
{
    if (provider == "iptv" && id && std::strncmp(id, "c:", 2) == 0) {
        const std::string name = focused_chan_key();
        if (!name.empty()) return "evo://iptv/n:" + name;
    }
    return "evo://" + provider + "/" + (id ? id : "");
}

/* "g:5", "vc:7", "sc:9" - a category that can be hidden and moved. */
bool is_category(const char* inner)
{
    const char* rest = nullptr;
    if (std::strncmp(inner, "g:", 2) == 0) rest = inner + 2;
    else if (std::strncmp(inner, "vc:", 3) == 0 || std::strncmp(inner, "sc:", 3) == 0) rest = inner + 3;
    return rest && *rest && std::strcmp(rest, "*") != 0 && rest[0] != '@';
}

/* A category the parental lock can hold: Stalker's, or Xtream's "vod_cat:7". */
bool is_lockable(const char* inner)
{
    return is_category(inner) || std::strstr(inner, "_cat:") != nullptr;
}

/* ---- the parental lock ---------------------------------------------------- */

struct PinWait {
    std::string id, title;
    int what = 0;               /* 0 open the folder, 1 unlock the category */
    std::string provider;
};
PinWait g_pinWait;

void onParentalPin(const char* text, void*)
{
    if (!evo_parental_unlock(text ? text : "")) {
        evo_feedback(EVO_FB_ERROR);
        toast("PARENTAL LOCK", "Wrong PIN");
        return;
    }
    evo_feedback(EVO_FB_CONFIRM);
    if (g_pinWait.what == 1) {
        evo_parental_set_locked(g_pinWait.provider.c_str(), g_pinWait.id.c_str(), 0);
        toast("PARENTAL LOCK", "Category unlocked");
        evo_rmlui_provider_reload();
    } else {
        toast("PARENTAL LOCK", "Unlocked for two hours");
        evo_rmlui_provider_open_folder_now(g_pinWait.id.c_str(), g_pinWait.title.c_str());
    }
}

void askPin(const char* provider, const char* id, const char* title, int what)
{
    g_pinWait.provider = provider ? provider : "";
    g_pinWait.id = id ? id : "";
    g_pinWait.title = title ? title : "";
    g_pinWait.what = what;
    evo_feedback(EVO_FB_OPEN);
    evo_keyboard_open("Parental lock - type the PIN", "", 8, &onParentalPin, nullptr);
}

/* Asked by the list before any folder opens. */
int parentalGate(const char* provider, const char* id, const char* title)
{
    if (!evo_parental_has_pin() || evo_parental_unlocked()) return 1;
    const char* in = inner_id(id);
    if (!is_lockable(in) || !evo_parental_category_locked(provider, in, title)) return 1;
    askPin(provider, id, title, 0);
    return 0;
}
} // namespace

void ProviderHostScreen::toggleFavoriteFocused()
{
    const char* title = evo_rmlui_provider_get_focused_title();
    const char* id = evo_rmlui_provider_get_focused_id();
    if (!title || !title[0] || !id || !id[0]) return;
    /*
     * A favourite is kept by its identity, "evo://<provider>/<id>", the same
     * key Continue Watching uses - never by its stream URL, which carried the
     * account and stopped working once its token expired.
     */
    const std::string key = fav_key(m_providerId, id);
    const std::string name = m_providerId == "iptv" ? focused_chan_key() : std::string();
    if (!name.empty()) title = name.c_str();
    int idx = favorites_find(key.c_str());
    if (idx < 0) idx = favorites_find(title);           /* an entry from before this */
    if (idx >= 0) {
        favorites_remove(favorite_files[idx].path);
        favorites_save();
        toast("FAVORITES", "Removed from favorites");
        evo_feedback(EVO_FB_CANCEL);
    } else {
        favorites_add(key.c_str(), title, 0.0);
        favorites_set_art(key.c_str(), evo_rmlui_provider_get_focused_art());
        favorites_save();
        toast("FAVORITES", "Added to favorites");
        evo_feedback(EVO_FB_CONFIRM);
    }
    evo_rmlui_provider_reload();
}

void ProviderHostScreen::toggleWatchedFocused()
{
    const char* id = evo_rmlui_provider_get_focused_id();
    const char* title = evo_rmlui_provider_get_focused_title();
    if (!id || !id[0] || std::strstr(id, "ch:") || std::strstr(id, "live_stream:"))
        return;                                      /* a channel has nothing to finish */
    const std::string key = "evo://" + m_providerId + "/" + id;
    int at = -1;
    for (int i = 0; i < recent_file_count; ++i)
        if (key == recent_files[i].path) { at = i; break; }
    const bool watched = at >= 0 && recent_files[at].duration > 1.0 &&
                         recent_files[at].last_pos / recent_files[at].duration >= 0.95;
    if (watched) {
        recent_remove_at(at);
        toast("WATCHED", "Marked as not watched");
        evo_feedback(EVO_FB_CANCEL);
    } else {
        const double d = (at >= 0 && recent_files[at].duration > 1.0) ? recent_files[at].duration : 100.0;
        recent_add_or_update(key.c_str(), title ? title : "", d, d);
        recent_save();
        toast("WATCHED", "Marked as watched");
        evo_feedback(EVO_FB_CONFIRM);
    }
    evo_rmlui_provider_reload();
}

void ProviderHostScreen::openActionMenu()
{
    const char* id = evo_rmlui_provider_get_focused_id();
    const char* title = evo_rmlui_provider_get_focused_title();
    const bool folder = evo_rmlui_provider_get_focused_is_folder() != 0;
    const char* in = inner_id(id);
    m_actions.clear();
    m_actionIndex = 0;
    m_actionTitle = (title && title[0]) ? title : "Options";

    auto add = [this](int code, const char* label, const char* detail) {
        m_actions.push_back({code, label, detail});
    };

    const bool stalker = m_providerId == "stalker";      /* hide / move / sort are Stalker's */
    const bool iptv = m_providerId == "iptv";
    const bool searching = EvoRmlProviderHost::Instance().SearchShown();
    if (id && id[0] && !folder) {
        const bool channel = is_channel_id(in);
        if (channel) {
            add(ActPlay, "Watch live", "Start this channel");
            add(ActGrid, "TV guide grid", "Every channel of this list against the clock");
            if (!iptv)
                add(ActGuide, "TV guide", "What is on today and the next days - and catch-up");
        } else {
            /* Where Continue Watching has it: resume, or start again. */
            const std::string key = "evo://" + m_providerId + "/" + id;
            double pos = 0.0, dur = 0.0;
            for (int i = 0; i < recent_file_count; ++i)
                if (key == recent_files[i].path) { pos = recent_files[i].last_pos; dur = recent_files[i].duration; break; }
            const bool watched = dur > 1.0 && pos / dur >= 0.95;
            static char resumeLabel[48];
            if (pos > 1.0 && !watched) {
                const long s = static_cast<long>(pos);
                if (s >= 3600) std::snprintf(resumeLabel, sizeof resumeLabel, "Resume from %ld:%02ld:%02ld",
                                             s / 3600, (s / 60) % 60, s % 60);
                else           std::snprintf(resumeLabel, sizeof resumeLabel, "Resume from %ld:%02ld", s / 60, s % 60);
                add(ActPlay, resumeLabel, "Carry on where you stopped");
                add(ActPlayStart, "Play from the beginning", "Start again at 0:00");
            } else {
                add(ActPlay, "Play", watched ? "You have seen this - it starts at the beginning" : "Start playing");
            }
            add(ActWatched, watched ? "Mark as not watched" : "Mark as watched",
                "The check mark in this list and the tile on Home");
        }
        const std::string fkey = fav_key(m_providerId, id);
        const bool fav = favorites_find(fkey.c_str()) >= 0;
        add(ActFavorite, fav ? "Remove from favorites" : "Add to favorites",
            channel ? "Favorite channels, at the top of this list" : "Favorites on the side menu");
        if (channel && !searching && !focused_chan_key().empty()) {
            add(ActChanHide, "Hide this channel", "Bring it back with Show hidden channels");
            if (!stalker) {
                add(ActChanUp, "Move up", "One place up in this list (L2)");
                add(ActChanDown, "Move down", "One place down in this list (R2)");
                add(ActChanMoveTo, "Move to position...", "Put this channel where you want it in the list");
                add(ActChanResetOrder, "Original order", "This list as the provider sends it");
            }
        }
        if (stalker && channel) add(ActMoveTo, "Move to position...", "Put this channel where you want it in the list");
        if (stalker) add(ActSort, "Change the order", "A-Z, Z-A, channel number, the portal's order");
    } else if (id && id[0]) {
        add(ActOpen, "Open", "Show what is in it");
        if (std::strncmp(id, "acct:", 5) == 0 && m_providerId == "xtream")
            add(ActRemoveSource, "Remove this account", "Kijkr forgets this server and its login");
        if (std::strncmp(id, "pl:", 3) == 0 && iptv)
            add(ActRemoveSource, "Remove this playlist", "Kijkr forgets this playlist");
        if (is_lockable(in)) {
            if (evo_parental_category_locked(m_providerId.c_str(), in, title))
                add(ActUnlock, "Unlock this category", "No PIN needed to open it any more");
            else
                add(ActLock, "Lock this category", "Only opens with the parental PIN (Settings - Home & Content)");
        }
        if (stalker && is_category(in)) {
            add(ActHide, "Hide this category", "Bring it back with Show hidden categories");
            add(ActMoveUp, "Move up", "One place up in the list");
            add(ActMoveDown, "Move down", "One place down in the list");
            add(ActMoveTo, "Move to position...", "Put it where you want it in the list");
        }
    }
    add(ActSearch, "Search", stalker ? "Films, series and channels on this portal"
                                     : "Films, series and channels on this account");
    if (evo_parental_has_pin() && evo_parental_unlocked())
        add(ActRelock, "Lock again now", "Locked categories ask for the PIN again");
    if (evo_chanprefs_hidden_count(m_providerId.c_str()) > 0)
        add(ActChanShowHidden, "Show hidden channels", "Every channel you hid comes back");
    if (stalker) {
        add(ActRestoreHidden, "Show hidden categories", "Everything you hid comes back");
        add(ActPortalSettings, "Portal settings", "Add, change or remove a portal and its MAC address");
    } else if (iptv) {
        add(ActPlaylistMenu, "Playlist & guide", "Add a playlist, choose the TV guide");
    } else {
        add(ActPortalSettings, "Account settings", "Add an account, or change the server, user name and password");
    }

    m_actionMenu = true;
    evo_feedback(EVO_FB_OPEN);
}

void ProviderHostScreen::runAction(int code)
{
    m_actionMenu = false;
    switch (code) {
    case ActPlay:
    case ActOpen:
        if (m_resolving || m_tunePending || g_start_running) return;
        evo_feedback(EVO_FB_OPEN);
        evo_rmlui_provider_key(EvoRmlProviderHost::KeyAccept);
        break;
    case ActPlayStart: {
        const char* id = evo_rmlui_provider_get_focused_id();
        const char* title = evo_rmlui_provider_get_focused_title();
        if (!id || !id[0] || m_resolving || g_start_running) return;
        /* The same path a resume from Home takes, at 0:00. */
        g_launchResume.active = true;
        g_launchResume.provider = m_providerId;
        g_launchResume.item_id = id;
        g_launchResume.title = (title && title[0]) ? title : id;
        g_launchResume.resume_sec = 0;
        evo_feedback(EVO_FB_OPEN);
        break;
    }
    case ActWatched:        toggleWatchedFocused(); break;
    case ActFavorite:       toggleFavoriteFocused(); break;
    case ActHide:           stalkerItemAction(STALKER_ACT_HIDE); break;
    case ActMoveUp:         stalkerItemAction(STALKER_ACT_UP); break;
    case ActMoveDown:       stalkerItemAction(STALKER_ACT_DOWN); break;
    case ActMoveTo:         stalkerMovePrompt(); break;
    case ActSort:           stalkerSortAction(1); break;
    case ActSearch:         openSearch(); break;
    case ActRestoreHidden: {
        char msg[120] = {0};
        stalker_restore_categories(msg, sizeof msg);
        toast("STALKER / MINISTRA", msg[0] ? msg : "Hidden categories are back");
        evo_rmlui_provider_reload();
        break;
    }
    case ActPortalSettings:
        evo_feedback(EVO_FB_OPEN);
        evo_rmlui_provider_show_setup();
        break;
    case ActLock: {
        const char* id = evo_rmlui_provider_get_focused_id();
        evo_parental_set_locked(m_providerId.c_str(), inner_id(id), 1);
        evo_feedback(EVO_FB_CONFIRM);
        toast("PARENTAL LOCK", evo_parental_has_pin() ? "Category locked"
                                                      : "Locked - set a PIN in Settings > Interface to use it");
        evo_rmlui_provider_reload();
        break;
    }
    case ActUnlock: {
        const char* id = evo_rmlui_provider_get_focused_id();
        if (evo_parental_unlocked()) {
            evo_parental_set_locked(m_providerId.c_str(), inner_id(id), 0);
            evo_feedback(EVO_FB_CONFIRM);
            toast("PARENTAL LOCK", "Category unlocked");
            evo_rmlui_provider_reload();
        } else {
            askPin(m_providerId.c_str(), inner_id(id), evo_rmlui_provider_get_focused_title(), 1);
        }
        break;
    }
    case ActGuide: {
        const std::string id = evo_rmlui_provider_get_focused_id();
        const std::string title = evo_rmlui_provider_get_focused_title();
        std::string folder;
        const size_t ch = id.find("ch:");
        const size_t ls = id.find("live_stream:");
        if (ch != std::string::npos)      folder = id.substr(0, ch) + "gd:" + id.substr(ch + 3);
        else if (ls != std::string::npos) folder = id.substr(0, ls) + "xg:" + id.substr(ls + 12);
        if (folder.empty()) break;
        evo_feedback(EVO_FB_OPEN);
        evo_rmlui_provider_open_folder(folder.c_str(), ("TV guide - " + title).c_str());
        break;
    }
    case ActChanHide:       chanHide(); break;
    case ActChanUp:         chanMove(-1, true); break;
    case ActChanDown:       chanMove(+1, true); break;
    case ActChanMoveTo:     chanMovePrompt(); break;
    case ActChanResetOrder:
        evo_chanprefs_reset_order(m_providerId.c_str(), EvoRmlProviderHost::Instance().CurrentParent().c_str());
        evo_feedback(EVO_FB_CONFIRM);
        toast("CHANNELS", "Back in the original order");
        evo_rmlui_provider_reload();
        break;
    case ActChanShowHidden: {
        const int n = evo_chanprefs_unhide_all(m_providerId.c_str());
        char msg[64];
        std::snprintf(msg, sizeof msg, n == 1 ? "1 channel is back" : "%d channels are back", n);
        evo_feedback(EVO_FB_CONFIRM);
        toast("CHANNELS", msg);
        evo_rmlui_provider_reload();
        break;
    }
    case ActRemoveSource: {
        const char* id = evo_rmlui_provider_get_focused_id();
        if (!id) break;
        int r = -1;
        if (std::strncmp(id, "acct:", 5) == 0) r = xtream_remove_account(std::atoi(id + 5));
        else if (std::strncmp(id, "pl:", 3) == 0) r = provider_iptv_remove_playlist(std::atoi(id + 3));
        evo_feedback(r == 0 ? EVO_FB_CONFIRM : EVO_FB_ERROR);
        toast(m_providerId == "xtream" ? "XTREAM CODES" : "M3U PLAYLIST",
              r == 0 ? "Removed" : "Could not remove it");
        evo_rmlui_provider_reload();
        break;
    }
    case ActPlaylistMenu:
        openProviderMenu();
        break;
    case ActGrid:
        openGrid();
        break;
    case ActRelock:
        evo_parental_relock();
        evo_feedback(EVO_FB_CONFIRM);
        toast("PARENTAL LOCK", "Locked again");
        break;
    case ActSearchNew:
        openSearchKeyboard();
        break;
    case ActSearchClear:
        evo_shist_clear();
        evo_feedback(EVO_FB_CONFIRM);
        toast("SEARCH", "Search history cleared");
        break;
    case ActSearchForget: {
        /* The same list again, each entry now removing itself. */
        const int nh = evo_shist_count();
        m_actions.clear();
        m_actionIndex = 0;
        m_actionTitle = "Remove a search";
        for (int i = 0; i < nh; ++i)
            m_actions.push_back({ActHistory + 100 + i, evo_shist_get(i), "Remove from the list"});
        if (m_actions.empty()) break;
        m_actionMenu = true;
        evo_feedback(EVO_FB_OPEN);
        break;
    }
    default:
        if (code >= ActHistory + 100) {               /* remove one */
            evo_shist_remove(code - ActHistory - 100);
            evo_feedback(EVO_FB_CONFIRM);
            toast("SEARCH", "Removed from the search history");
            if (evo_shist_count() > 0) runAction(ActSearchForget);
        } else if (code >= ActHistory) {              /* search again */
            const std::string q = evo_shist_get(code - ActHistory);
            if (q.empty()) break;
            m_searchQuery = q;
            evo_shist_add(q.c_str());
            evo_feedback(EVO_FB_CONFIRM);
            evo_rmlui_provider_search(q.c_str());
        }
        break;
    }
}

void ProviderHostScreen::renderActionMenu(uint32_t* framebuffer, int width, int height)
{
    evo_rmlui_list_params_t params;
    std::memset(&params, 0, sizeof(params));
    params.section = EVO_SECTION_EMBY;
    params.rail_focused = 0;
    params.title = "MENU";
    static char subtitle[EVO_PROVIDER_MAX_TITLE];
    std::snprintf(subtitle, sizeof subtitle, "%s", m_actionTitle.c_str());
    params.subtitle = subtitle;

    const int total = static_cast<int>(m_actions.size());
    params.total_count = total;
    params.cursor_index = total ? m_actionIndex : -1;
    /* The list shows EVO_RMLUI_LIST_ROWS at a time: scroll with the cursor. */
    const int rows = std::min(EVO_RMLUI_LIST_ROWS, total);
    int first = 0;
    if (m_actionIndex >= rows) first = m_actionIndex - rows + 1;
    params.row_count = rows;
    for (int i = 0; i < rows; ++i) {
        const MenuAction& a = m_actions[first + i];
        params.rows[i].title = a.label.c_str();
        params.rows[i].detail = a.detail.c_str();
        params.rows[i].icon_path = (a.code == ActPortalSettings) ? "../icons/icon_settings.png"
                                 : (a.code == ActFavorite)       ? "../icons/icon_favorites.png"
                                 : (a.code == ActSearch || a.code == ActSearchNew ||
                                    a.code >= ActHistory)        ? "../icons/icon_type.png"
                                 : (a.code == ActSearchClear ||
                                    a.code == ActSearchForget)   ? "../icons/icon_trash.png"
                                 : (a.code == ActWatched)        ? "../icons/icon_resume.png"
                                 : (a.code == ActHide)           ? "../icons/icon_trash.png"
                                 : (a.code == ActGuide)          ? "../icons/icon_tv.png"
                                 : (a.code == ActLock || a.code == ActUnlock ||
                                    a.code == ActRelock)         ? "../icons/icon_power.png"
                                 :                                 "../icons/icon_chevron.png";
        params.rows[i].badge = "";
        params.rows[i].progress = -1;
        params.rows[i].has_chevron = 1;
        params.rows[i].is_focused = (first + i == m_actionIndex);
    }
    params.hint_count = 2;
    params.hints[0].glyph_path = "../icons/btn_cross.png";
    params.hints[0].label = "SELECT";
    params.hints[1].glyph_path = "../icons/btn_circle.png";
    params.hints[1].label = "BACK";
    evo_rmlui_update_list(&params);
    evo_rmlui_render_list(framebuffer, width, height);
}

bool ProviderHostScreen::handleInput(uint32_t pressed, uint32_t held, uint32_t released)
{
    (void)held;
    (void)released;

    /* Discovery takes ~1.5 s at most and then opens the keyboard itself; a
     * sign-in in flight finishes or fails on its own the same way. */
    if (m_discovering) return true;
    if (m_signIn == SignIn::Suggest || m_signIn == SignIn::Signing) return true;

    if (m_streamPick) {
        const int n = g_pending.choice_count;
        if (pressed & PadButtons::Up) {
            if (m_streamIndex > 0) { --m_streamIndex; evo_feedback(EVO_FB_MOVE); }
            else evo_feedback(EVO_FB_BOUNDARY);
            return true;
        }
        if (pressed & PadButtons::Down) {
            if (m_streamIndex + 1 < n) { ++m_streamIndex; evo_feedback(EVO_FB_MOVE); }
            else evo_feedback(EVO_FB_BOUNDARY);
            return true;
        }
        if (pressed & PadButtons::Cross) {
            chooseStream(m_streamIndex);
            return true;
        }
        if (pressed & PadButtons::Circle) {
            evo_feedback(EVO_FB_CANCEL);
            cancelStreamPicker();
            return true;
        }
        return true;        /* the list has the pad while it is open */
    }
    if (g_grid.open && m_opened) return gridInput(pressed);
    if (m_actionMenu) {
        const int n = static_cast<int>(m_actions.size());
        if (pressed & PadButtons::Up) {
            if (m_actionIndex > 0) { --m_actionIndex; evo_feedback(EVO_FB_MOVE); }
            else evo_feedback(EVO_FB_BOUNDARY);
        } else if (pressed & PadButtons::Down) {
            if (m_actionIndex + 1 < n) { ++m_actionIndex; evo_feedback(EVO_FB_MOVE); }
            else evo_feedback(EVO_FB_BOUNDARY);
        } else if (pressed & PadButtons::Cross) {
            if (m_actionIndex >= 0 && m_actionIndex < n) runAction(m_actions[m_actionIndex].code);
        } else if (pressed & (PadButtons::Circle | PadButtons::Options)) {
            evo_feedback(EVO_FB_CANCEL);
            m_actionMenu = false;
        }
        return true;
    }
    if (signInQcInput(pressed)) return true;

    if (m_picking) {
        if (!m_menu.empty()) return pickerMenuInput(pressed);
        auto psm = Application::getInstance().getScreenManager();
        if (psm && psm->isRailFocused()) return false;
        int n = (int)m_pickIds.size();
        if (pressed & PadButtons::Up) {
            if (m_pickIndex > 0) { --m_pickIndex; evo_feedback(EVO_FB_MOVE); }
            else evo_feedback(EVO_FB_BOUNDARY);
            return true;
        }
        if (pressed & PadButtons::Down) {
            if (m_pickIndex + 1 < n) { ++m_pickIndex; evo_feedback(EVO_FB_MOVE); }
            else evo_feedback(EVO_FB_BOUNDARY);
            return true;
        }
        if (pressed & PadButtons::Left) {
            if (psm) { psm->setRailFocused(true); return true; }
            return false;
        }
        if (pressed & PadButtons::Cross)   { choosePicked(false); return true; }
        if (pressed & PadButtons::Square)  { choosePicked(true);  return true; }
        if (pressed & PadButtons::Triangle) {
            /* The site itself, in the system browser - for what EVO's screens
             * do not cover (server settings, admin). */
            const evo_provider_t* tp = (m_pickIndex >= 0 && m_pickIndex < n)
                                     ? evo_provider_find(m_pickIds[m_pickIndex].c_str()) : nullptr;
            if (tp && isNativeWeb(tp) && tp->is_configured()) {
                evo_feedback(EVO_FB_OPEN);
                m_providerId = tp->id;
                m_picking = false;
                openWebProvider();
                if (!m_web) enterPicker();
            }
            return true;
        }
        if (pressed & PadButtons::Options) {
            openPickerMenu();
            return true;
        }
        if (pressed & PadButtons::Circle) {
            evo_feedback(EVO_FB_CANCEL);
            if (psm) psm->navigateTo(ScreenId::MainMenu);
            return true;
        }
        return false;
    }
    if (!m_opened) return false;

    auto sm = Application::getInstance().getScreenManager();
    bool railFocused = sm && sm->isRailFocused();

    /* While the rail has focus the provider document must not move: the rail
     * owns Up/Down and Left/Right in that state, exactly as on every other
     * screen. */
    if (railFocused) return false;

    /* The Live TV options panel: the d-pad and X work its rows, Circle steps back,
     * Options closes it, and nothing may act on the grid behind it. */
    if (evo_rmlui_provider_panel_open()) {
        if (pressed & PadButtons::Options) {
            evo_feedback(EVO_FB_CANCEL);
            evo_rmlui_provider_hide_panel();
        } else if (pressed & PadButtons::Up) {
            evo_feedback(EVO_FB_MOVE);
            evo_rmlui_provider_key(EvoRmlProviderHost::KeyUp);
        } else if (pressed & PadButtons::Down) {
            evo_feedback(EVO_FB_MOVE);
            evo_rmlui_provider_key(EvoRmlProviderHost::KeyDown);
        } else if (pressed & PadButtons::Cross) {
            evo_feedback(EVO_FB_CONFIRM);
            evo_rmlui_provider_key(EvoRmlProviderHost::KeyAccept);
        } else if (pressed & PadButtons::Circle) {
            evo_feedback(EVO_FB_CANCEL);
            evo_rmlui_provider_key(EvoRmlProviderHost::KeyBack);
        }
        return true;
    }

    /*
     * Everything below goes into RmlUi. EVO does not translate a direction into
     * a row index anywhere in this function - that is the difference between
     * this screen and every other one in the tree.
     */
    if (pressed & PadButtons::Up)
        return evo_rmlui_provider_key(EvoRmlProviderHost::KeyUp) != 0;
    if (pressed & PadButtons::Down)
        return evo_rmlui_provider_key(EvoRmlProviderHost::KeyDown) != 0;
    if (pressed & PadButtons::Right)
        return evo_rmlui_provider_key(EvoRmlProviderHost::KeyRight) != 0;
    if (pressed & PadButtons::L1) {
        evo_feedback(EVO_FB_MOVE);
        return evo_rmlui_provider_key(EvoRmlProviderHost::KeyPageUp) != 0;
    }
    if (pressed & PadButtons::R1) {
        evo_feedback(EVO_FB_MOVE);
        return evo_rmlui_provider_key(EvoRmlProviderHost::KeyPageDown) != 0;
    }

    if (pressed & PadButtons::Left) {
        /* Let the document have it first - a grid's leftmost column is where
         * the rail should take over, and only the document knows where that
         * is. An unconsumed Left focuses the rail. */
        if (evo_rmlui_provider_key(EvoRmlProviderHost::KeyLeft) != 0)
            return true;
        if (sm) { sm->setRailFocused(true); return true; }
        return false;
    }

    if (pressed & PadButtons::Cross) {
        if (m_resolving || m_tunePending || g_start_running) return true;      /* one activation at a time */
        evo_feedback(EVO_FB_OPEN);
        return evo_rmlui_provider_key(EvoRmlProviderHost::KeyAccept) != 0;
    }

    if (pressed & PadButtons::Circle) {
        if (m_resolving || m_tunePending || g_start_running) return true;
        evo_feedback(EVO_FB_CANCEL);
        /*
         * The exit contract. The host consumes Back while it is deeper than the
         * provider's root and pops a level; at the root it declines, and the
         * screen leaves. A provider that swallowed Back at its root would be a
         * screen the user cannot get out of, and the only remaining way out is
         * the PS button - the close path that panicked the console.
         */
        if (evo_rmlui_provider_key(EvoRmlProviderHost::KeyBack) != 0)
            return true;
        /* Out of a provider's root: back to the chooser when there is one to
         * go back to, so switching provider is one Circle away rather than
         * a trip through the main menu. */
        if (pickable_count(nullptr) > 1) {
            evo_feedback(EVO_FB_CANCEL);
            enterPicker();
            return true;
        }
        if (sm) sm->navigateTo(ScreenId::MainMenu);
        return true;
    }

    if ((pressed & PadButtons::R3) && m_providerId == "stalker") {
        stalkerMovePrompt();
        return true;
    }

    /* Search results: L2 / R2 step through all, films, series and live TV. */
    if ((pressed & (PadButtons::L2 | PadButtons::R2)) &&
        evo_rmlui_provider_search_tab((pressed & PadButtons::L2) ? -1 : 1)) {
        evo_feedback(EVO_FB_MOVE);
        return true;
    }

    if ((pressed & (PadButtons::L2 | PadButtons::R2)) && m_providerId == "stalker") {
        if (evo_rmlui_provider_get_focused_is_folder())
            stalkerItemAction((pressed & PadButtons::L2) ? STALKER_ACT_UP : STALKER_ACT_DOWN);
        else
            stalkerSortAction((pressed & PadButtons::L2) ? -1 : 1);
        return true;
    }

    /* L2 / R2 on a channel of a playlist or an Xtream list: one place up / down. */
    if ((pressed & (PadButtons::L2 | PadButtons::R2)) &&
        (m_providerId == "iptv" || m_providerId == "xtream") && !focused_chan_key().empty()) {
        chanMove((pressed & PadButtons::L2) ? -1 : 1, true);
        return true;
    }

    if (pressed & PadButtons::Square) {
        openSearch();
        return true;
    }

    /* Touchpad on a channel: the TV guide grid. */
    if ((pressed & PadButtons::TouchPad) && !evo_rmlui_provider_get_focused_is_folder() &&
        is_channel_id(inner_id(evo_rmlui_provider_get_focused_id()))) {
        openGrid();
        return true;
    }

    /*
     * Touchpad click on a film or an episode: watched / not watched. It is the
     * same entry Continue Watching keeps, so the check mark, Home and "up
     * next" all agree with it.
     */
    if ((pressed & PadButtons::TouchPad) && !evo_rmlui_provider_get_focused_is_folder()) {
        toggleWatchedFocused();
        return true;
    }

    if (pressed & PadButtons::Triangle) {
        if (m_providerId == "stalker" && evo_rmlui_provider_get_focused_is_folder()) {
            stalkerItemAction(STALKER_ACT_HIDE);        /* hide a category, remove a portal */
            return true;
        }
        if (!evo_rmlui_provider_get_focused_is_folder()) {
            const char* title = evo_rmlui_provider_get_focused_title();
            const char* id = evo_rmlui_provider_get_focused_id();
            if (title && title[0] && id && id[0]) {
                toggleFavoriteFocused();
                return true;
            }
        }
    }

    /*
     * OPTIONS, not Triangle.
     *
     * Triangle is the virtual keyboard's own "Quick Done" (evo_keyboard.c),
     * so opening the editor with it submitted the prompt on the spot: the
     * hardware log showed seven "source set to <unchanged url>" lines from
     * seven presses, each one closing and reopening the host - which is what
     * the flashing was. Options is free on this screen and is where a console
     * user looks for settings anyway.
     */
    if ((pressed & PadButtons::Options) &&
        (m_providerId == "stalker" || m_providerId == "xtream" || m_providerId == "iptv") && m_opened) {
        openActionMenu();
        return true;
    }
    if (pressed & PadButtons::Options) {
        evo_feedback(EVO_FB_OPEN);
        const evo_provider_t* op = evo_provider_find(m_providerId.c_str());
        if (op && (isNativeWeb(op) || op->ui_embedded)) {
            /* No setup page: the chooser's menu for this provider is where
             * the address is edited, the user switched and the web version
             * opened. */
            std::string id = op->id;
            enterPicker();
            for (int i = 0; i < (int)m_pickIds.size(); ++i)
                if (m_pickIds[i] == id) m_pickIndex = i;
            if (m_picking) openPickerMenu();
        } else if (op && std::strcmp(op->id, "iptv") == 0 &&
                   !evo_rmlui_provider_get_query()[0]) {
            openProviderMenu();     /* playlist or guide, for the one on screen */
        } else {
            evo_rmlui_provider_show_setup();
        }
        return true;
    }

    return false;
}

/* ------------------------------------------------------------------------- */
/* Search                                                                    */
/* ------------------------------------------------------------------------- */

void ProviderHostScreen::openSearch()
{
    const evo_provider_t* p = evo_provider_find(m_providerId.c_str());
    if (!p || !(p->caps & EVO_PROVIDER_CAP_SEARCH)) {
        toast("SEARCH", "This provider does not support search");
        return;
    }

    /* The last searches first, when there are any: one press to repeat one. */
    const int nh = evo_shist_count();
    if (nh > 0) {
        m_actions.clear();
        m_actionIndex = 0;
        m_actionTitle = "Search";
        m_actions.push_back({ActSearchNew, "New search...", "Type what to look for"});
        for (int i = 0; i < nh; ++i)
            m_actions.push_back({ActHistory + i, evo_shist_get(i), "Search again"});
        m_actions.push_back({ActSearchForget, "Remove one search...", "Choose a search to take off this list"});
        m_actions.push_back({ActSearchClear, "Clear search history", "Remove every search from this list"});
        m_actionMenu = true;
        evo_feedback(EVO_FB_OPEN);
        return;
    }
    openSearchKeyboard();
}

void ProviderHostScreen::openSearchKeyboard()
{
    evo_feedback(EVO_FB_OPEN);
    evo_keyboard_open("Search films, series and channels...", m_searchQuery.c_str(), 64,
                      &ProviderHostScreen::OnSearchSubmitted, this);
}

void ProviderHostScreen::OnSearchSubmitted(const char* text, void* userdata)
{
    auto* self = static_cast<ProviderHostScreen*>(userdata);
    if (!self) return;

    std::string value = text ? text : "";
    size_t b = value.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) {
        value.clear();
    } else {
        size_t e = value.find_last_not_of(" \t\r\n");
        value = value.substr(b, e - b + 1);
    }

    self->m_searchQuery = value;
    if (!value.empty()) evo_shist_add(value.c_str());
    evo_rmlui_provider_search(value.c_str());
}

/* ------------------------------------------------------------------------- */
/* Source editing - the typed M3U URL                                        */
/* ------------------------------------------------------------------------- */

void ProviderHostScreen::OnSourceSubmitted(const char* text, void* userdata)
{
    auto* self = static_cast<ProviderHostScreen*>(userdata);
    if (!self) return;

    const evo_provider_t* p = evo_provider_find(self->m_providerId.c_str());
    if (!p || !(p->caps & EVO_PROVIDER_CAP_CONFIG) || !p->set_source) return;

    /* Trim - a keyboard picks up trailing spaces very easily, and a URL with
     * one on the end fails in a way that looks like the URL being wrong. */
    std::string value = text ? text : "";
    size_t b = value.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) {
        value.clear();
    } else {
        size_t e = value.find_last_not_of(" \t\r\n");
        value = value.substr(b, e - b + 1);
    }

    if (std::strcmp(p->id, "stalker") == 0 && value.empty()) {
        toast(p->name, "Nothing entered - no portal added");
        return;
    }
    if (std::strcmp(p->id, "stalker") == 0 && !value.empty() &&
        value.find_first_of(" ;|,") == std::string::npos) {
        /* Only the portal URL so far: the MAC address is asked next, on its own
         * keyboard, opened from update(). */
        self->m_stalkerUrl = value;
        self->m_stalkerStep = StalkerStep::OpenMac;
        return;
    }

    bool web = (p->caps & EVO_PROVIDER_CAP_WEBUI) != 0;
    if (web && p->get_source && p->is_configured() && value == p->get_source()) {
        /* The same address again: setting it would drop the session. */
        toast(p->name, "Server unchanged");
        return;
    }
    if (p->set_source(value.c_str()) != 0) {
        toast(p->name, web ? "Use http(s)://<host>:<port>"
                           : std::strcmp(p->id, "stalker") == 0
                                 ? "Check the portal URL and the MAC address"
                                 : "That does not look like an M3U URL");
        return;
    }

    /* Persisted by the provider. Enable it now that it has a source. */
    evo_provider_set_enabled(p->id, p->is_configured() ? 1 : 0);
    self->m_picking = false;
    if (web) {
        /* #101: a web-UI provider opens straight away on its new address. A
         * media server browsed natively signs in first (openProvider). */
        toast(p->name, "Server saved");
        if (isNativeWeb(p))
            self->openProvider(p->id);
        else
            self->openWebProvider();
        return;
    }

    if (value.rfind("http://", 0) == 0 || value.rfind("https://", 0) == 0) {
        self->m_lastTypedUrl = value;
    }

    /* Reopen the host so the catalog is re-fetched from the new source. */
    if (p->ui_embedded)
        toast(p->name, value.empty() ? "Addons cleared" : "Addon added");
    else
        toast("PROVIDERS", std::strcmp(p->id, "stalker") == 0
                               ? (value.empty() ? "Portals removed" : "Portal added")
                               : (value.empty() ? "Playlist cleared" : "Playlist updated"));

    std::string id = self->m_providerId;
    if (self->m_opened) {
        evo_rmlui_provider_close();
        self->m_opened = false;
    }
    self->m_opened = evo_rmlui_provider_open(id.c_str(),
                                             DisplayWidth, DisplayHeight) != 0;
}

void ProviderHostScreen::OnGuideSubmitted(const char* text, void* userdata)
{
    (void)userdata;
    std::string value = text ? text : "";
    size_t b = value.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) {
        value.clear();
    } else {
        size_t e = value.find_last_not_of(" \t\r\n");
        value = value.substr(b, e - b + 1);
    }
    bool ok = value.empty() || value.rfind("http://", 0) == 0 ||
              value.rfind("https://", 0) == 0 || value[0] == '/';
    if (!ok) {
        toast("IPTV", "Guide must start with http(s):// or /");
        return;
    }
    if (provider_iptv_pin_guide(value.c_str()) != 0) {
        toast("IPTV", "Open a playlist first");
        return;
    }
    toast("LIVE TV", value.empty() ? "Guide: automatic" : "Guide saved for this playlist");
    evo_rmlui_provider_hide_panel();
    evo_rmlui_provider_reload();
}

static std::string baseName(const std::string& p)
{
    size_t s = p.find_last_of("/\\");
    return s == std::string::npos ? p : p.substr(s + 1);
}

/* What a guide is called on screen: a file's name, a web guide's host. */
static std::string guideLabel(const std::string& url)
{
    if (url.empty()) return "";
    if (url[0] == '/') return baseName(url);
    size_t b = url.find("://");
    b = (b == std::string::npos) ? 0 : b + 3;
    size_t e = url.find('/', b);
    return url.substr(b, e == std::string::npos ? std::string::npos : e - b);
}

static std::string ageText(long long s)
{
    char buf[48];
    if (s < 90)            return "just now";
    if (s < 3600)          std::snprintf(buf, sizeof buf, "%lld min ago", s / 60);
    else if (s < 2 * 86400) std::snprintf(buf, sizeof buf, "%lld h ago", s / 3600);
    else                    std::snprintf(buf, sizeof buf, "%lld days ago", s / 86400);
    return buf;
}

/* Owns the strings behind an evo_panel_t until it has been handed over. */
struct PanelBuild {
    struct Row { std::string id, title, detail, badge, icon; int radio = 0, on = 0, warn = 0, chevron = 0, live = 0; };
    std::vector<Row> rows;
    void show(const char* crumb, const char* eyebrow, const std::string& title,
              const std::string& sub, const char* note_b, const char* note,
              const char* accept, const char* back, int focus)
    {
        std::vector<evo_panel_row_t> r;
        for (const Row& x : rows)
            r.push_back({ x.id.c_str(), x.title.c_str(), x.detail.c_str(), x.badge.c_str(),
                          x.icon.c_str(), x.radio, x.on, x.warn, x.chevron, x.live });
        evo_panel_t p = { crumb, eyebrow, title.c_str(), sub.c_str(), note_b, note,
                          accept, back, r.data(), (int)r.size(), focus };
        evo_rmlui_provider_show_panel(&p);
    }
};

void ProviderHostScreen::openProviderMenu(int focus)
{
    evo_feedback(EVO_FB_OPEN);
    m_panelPage = "menu";

    const int n = provider_iptv_channel_count();
    const std::string pl = baseName(provider_iptv_playlist_name());
    const std::string pin = provider_iptv_pinned_guide();
    const std::string inUse = provider_iptv_guide_in_use();

    PanelBuild pb;
    PanelBuild::Row r;

    r.id = "menu:playlist"; r.title = "Playlist"; r.icon = "/assets/icons/icon_tv.png";
    r.detail = pl.empty() ? std::string("None yet")
                          : pl + " · " + std::to_string(n) + " channels";
    r.chevron = 1;
    pb.rows.push_back(r);

    r = PanelBuild::Row();
    r.id = "menu:guide"; r.title = "Channel guide"; r.icon = "/assets/icons/icon_recent_files.png";
    r.chevron = 1;
    if (pin == "none") {
        r.detail = "Turned off for this playlist";
    } else if (!inUse.empty()) {
        r.detail = guideLabel(inUse) + " · covers " + std::to_string(provider_iptv_guide_matched()) +
                   " of " + std::to_string(n) + " channels";
    } else {
        r.detail = "None yet - choose one";
        r.warn = 1;
    }
    pb.rows.push_back(r);

    r = PanelBuild::Row();
    r.id = "menu:refresh"; r.title = "Refresh guide"; r.icon = "/assets/icons/icon_activity.png";
    long long age = provider_iptv_guide_age();
    r.detail = inUse.empty() ? std::string("No guide loaded")
             : inUse[0] == '/' ? std::string("Reads the USB file again")
             : age >= 0 ? "Updated " + ageText(age) : std::string("Download it again");
    pb.rows.push_back(r);

    pb.show("", "LIVE TV", "Options", pl, "", "", "SELECT", "CLOSE", focus);
}

void ProviderHostScreen::openGuidePicker()
{
    evo_feedback(EVO_FB_OPEN);
    m_panelPage = "guide";

    const int n = provider_iptv_channel_count();
    const std::string pl = baseName(provider_iptv_playlist_name());
    const std::string pin = provider_iptv_pinned_guide();
    const std::string inUse = provider_iptv_guide_in_use();

    PanelBuild pb;
    int focus = 0;

    PanelBuild::Row a;
    a.id = "guide:auto"; a.title = "Automatic"; a.radio = 1; a.on = pin.empty();
    a.detail = (pin.empty() && !inUse.empty()) ? "Picked " + guideLabel(inUse)
                                               : std::string("Kijkr picks a guide that fits");
    pb.rows.push_back(a);

    /* Every guide EVO knows of for this playlist: files on the stick, the one in
     * use, the one chosen, the one in iptv.conf. */
    std::vector<std::string> urls;
    char found[16][512];
    int nf = provider_iptv_usb_guides(found, 16);
    for (int i = 0; i < nf; ++i) urls.emplace_back(found[i]);
    for (const std::string& u : { inUse, (pin == "none" ? std::string() : pin),
                                  std::string(provider_iptv_xmltv_url()) })
        if (!u.empty() && std::find(urls.begin(), urls.end(), u) == urls.end())
            urls.push_back(u);

    for (const std::string& u : urls) {
        PanelBuild::Row r;
        r.id = "guide:" + u;
        r.title = guideLabel(u);
        r.radio = 1;
        r.on = (pin == u);
        int cov = provider_iptv_guide_coverage(u.c_str());
        std::string where = u[0] == '/' ? "USB" : "Web";
        if (cov < 0)       r.detail = where + " · not tried yet";
        else if (cov == 0) { r.detail = where + " · covers none of these channels"; r.warn = 1; }
        else               r.detail = where + " · covers " + std::to_string(cov) + " of " +
                                      std::to_string(n) + " channels";
        if (u == inUse) { r.badge = "IN USE"; r.live = 1; }
        if (r.on) focus = (int)pb.rows.size();
        pb.rows.push_back(r);
    }

    PanelBuild::Row none;
    none.id = "guide:none"; none.title = "None"; none.radio = 1; none.on = (pin == "none");
    none.detail = "Show no guide for this playlist";
    if (none.on) focus = (int)pb.rows.size();
    pb.rows.push_back(none);

    PanelBuild::Row url;
    url.id = "guide:url"; url.title = "Enter guide address…";
    url.detail = "Web address of an XMLTV guide";
    url.icon = "/assets/icons/icon_keyboard.png"; url.chevron = 1;
    pb.rows.push_back(url);

    pb.show("OPTIONS  ›  ", "CHANNEL GUIDE", "Channel guide", "For " + pl,
            "Guides are not part of a playlist.",
            "Pick one once and Kijkr remembers it for this playlist. Put .xml guides on your "
            "USB stick and they show up here.",
            "USE THIS GUIDE", "BACK", focus);
}

void ProviderHostScreen::applyChoice(const std::string& id)
{
    if (id == "panel:back") {
        if (m_panelPage == "guide") openProviderMenu(1);
        else evo_rmlui_provider_hide_panel();
    } else if (id == "menu:playlist") {
        evo_rmlui_provider_hide_panel();
        evo_rmlui_provider_show_setup();
    } else if (id == "menu:guide") {
        openGuidePicker();
    } else if (id == "menu:refresh") {
        provider_iptv_refresh_guide();
        evo_rmlui_provider_hide_panel();
        toast("LIVE TV", "Refreshing the guide...");
    } else if (id == "guide:url") {
        const std::string pin = provider_iptv_pinned_guide();
        evo_keyboard_open("Guide address (XMLTV, http(s)://...)",
                          (!pin.empty() && pin[0] != '/' && pin != "none") ? pin.c_str() : "https://",
                          512, &ProviderHostScreen::OnGuideSubmitted, this);
    } else if (id.rfind("guide:", 0) == 0) {
        std::string choice = id.substr(6);
        std::string pinv = choice == "auto" ? std::string() : choice;
        if (provider_iptv_pin_guide(pinv.c_str()) != 0) {
            toast("LIVE TV", "Open a playlist first");
            return;
        }
        evo_rmlui_provider_hide_panel();
        toast("LIVE TV", choice == "auto" ? "Guide: automatic"
                       : choice == "none" ? "Guide turned off for this playlist"
                       : ("Guide: " + guideLabel(choice)).c_str());
        evo_rmlui_provider_reload();
    }
}

void ProviderHostScreen::openSourceEditor()
{
    const evo_provider_t* p = evo_provider_find(m_providerId.c_str());
    if (!p || !(p->caps & EVO_PROVIDER_CAP_CONFIG) || !p->get_source) {
        toast("PROVIDERS", "This provider has no playlist to set");
        return;
    }

    const char* current = p->get_source();
    std::string initial;
    if (current && (std::strncmp(current, "http://", 7) == 0 || std::strncmp(current, "https://", 8) == 0)) {
        initial = current;
    } else if (p->caps & EVO_PROVIDER_CAP_WEBUI) {
        /* Nothing set yet: Emby and Jellyfin answer a LAN broadcast, so look
         * first and pre-fill what answers - typing an address with the D-pad
         * took 103 presses on hardware. update() opens the keyboard. */
        if (m_providerId == "emby" || m_providerId == "jellyfin") {
            discoverServer();
            return;
        }
        initial = "http://";
    } else if (p->ui_embedded) {
        initial = "";               /* an addon to ADD, not a source to edit; the scheme is optional */
    } else if (!m_lastTypedUrl.empty()) {
        initial = m_lastTypedUrl;
    }
    openSourceKeyboard(initial);
}

void ProviderHostScreen::openSourceKeyboard(const std::string& initial)
{
    const evo_provider_t* p = evo_provider_find(m_providerId.c_str());
    if (!p) return;

    char title[96];
    std::string init = initial;
    if (std::strcmp(p->id, "stalker") == 0) {
        std::snprintf(title, sizeof title, "Add a portal - URL, e.g. http://host/c/");
        init = "http://";                           /* a new portal, not an edit of the last */
    } else if (p->caps & EVO_PROVIDER_CAP_WEBUI)
        std::snprintf(title, sizeof title, "%s server (http(s)://host:port)", p->name);
    else if (p->ui_embedded)
        std::snprintf(title, sizeof title, "Add a Stremio addon");
    else
        std::snprintf(title, sizeof title, "%s playlist URL (clear to reset)", p->name);

    evo_feedback(EVO_FB_OPEN);
    /* EVO_PROVIDER_MAX_URL is 2048, but a keyboard field that long is not
     * usable and no real M3U link needs it; 512 covers an Xtream get.php URL
     * with credentials and leaves the field navigable. */
    evo_keyboard_open_phone(title, init.c_str(), 512,
                            &ProviderHostScreen::OnSourceSubmitted, this);
}

std::vector<std::string> ProviderHostScreen::scanUsbPlaylists()
{
    std::vector<std::string> results;
    static const char* kRoots[] = { "/mnt/usb0", "/mnt/usb1", nullptr };

    auto isM3u = [](const std::string& name) -> bool {
        if (name.size() < 4) return false;
        std::string ext;
        size_t dot = name.find_last_of('.');
        if (dot == std::string::npos) return false;
        for (size_t i = dot; i < name.size(); ++i)
            ext += (char)std::tolower((unsigned char)name[i]);
        return (ext == ".m3u" || ext == ".m3u8");
    };

    for (int r = 0; kRoots[r]; ++r) {
        const char* root = kRoots[r];
        evo_dir_t* d = evo_opendir(root);
        if (!d) continue;

        struct dirent* entry;
        std::vector<std::string> subdirs;
        while ((entry = evo_readdir(d)) != nullptr) {
            if (entry->d_name[0] == '.') continue;
            if (std::strcmp(entry->d_name, "$RECYCLE.BIN") == 0) continue;
            if (std::strcmp(entry->d_name, "System Volume Information") == 0) continue;

            std::string name = entry->d_name;
            std::string fullPath = std::string(root) + "/" + name;

            if (entry->d_type == 4 /* DT_DIR */) {
                subdirs.push_back(fullPath);
            } else if (isM3u(name)) {
                results.push_back(fullPath);
            }
        }
        evo_closedir(d);

        for (const auto& sdir : subdirs) {
            evo_dir_t* sd = evo_opendir(sdir.c_str());
            if (!sd) continue;
            while ((entry = evo_readdir(sd)) != nullptr) {
                if (entry->d_name[0] == '.') continue;
                if (isM3u(entry->d_name)) {
                    results.push_back(sdir + "/" + entry->d_name);
                }
            }
            evo_closedir(sd);
        }
    }

#ifndef __PS5__
    /* On PC/host dev environment, also check current directory or sample files if /mnt/usb0 is absent */
    if (results.empty()) {
        static const char* kHostRoots[] = { ".", "assets", nullptr };
        for (int r = 0; kHostRoots[r]; ++r) {
            evo_dir_t* d = evo_opendir(kHostRoots[r]);
            if (!d) continue;
            struct dirent* entry;
            while ((entry = evo_readdir(d)) != nullptr) {
                if (entry->d_name[0] == '.') continue;
                if (isM3u(entry->d_name)) {
                    results.push_back(std::string(kHostRoots[r]) + "/" + entry->d_name);
                }
            }
            evo_closedir(d);
        }
    }
#endif

    return results;
}

void ProviderHostScreen::browseUsb()
{
    const evo_provider_t* p = evo_provider_find(m_providerId.c_str());
    if (!p || !(p->caps & EVO_PROVIDER_CAP_CONFIG) || !p->set_source) {
        toast("PROVIDERS", "This provider has no playlist to set");
        return;
    }

    evo_feedback(EVO_FB_OPEN);
    std::vector<std::string> playlists = scanUsbPlaylists();

    if (playlists.empty()) {
        toast("USB SCAN", "No M3U files found on USB");
        evo_rmlui_provider_set_status("No .m3u or .m3u8 files found on USB drive (/mnt/usb0, /mnt/usb1). Insert a USB drive with an M3U playlist.", 1);
        return;
    }

    if (playlists.size() == 1) {
        const std::string& path = playlists[0];
        if (p->set_source(path.c_str()) != 0) {
            toast("PROVIDERS", "Failed to load playlist from USB");
            return;
        }
        evo_provider_set_enabled(p->id, 1);
        size_t last_slash = path.find_last_of("/\\");
        std::string fname = (last_slash != std::string::npos) ? path.substr(last_slash + 1) : path;
        toast("IPTV USB", ("Loaded " + fname).c_str());

        /* Reopen provider so catalog is reloaded from USB */
        std::string id = m_providerId;
        if (m_opened) {
            evo_rmlui_provider_close();
            m_opened = false;
        }
        m_opened = evo_rmlui_provider_open(id.c_str(), DisplayWidth, DisplayHeight) != 0;
        return;
    }

    /* Multiple playlists found: present them as cards in the grid */
    EvoRmlProviderHost::Instance().ShowUsbPlaylists(playlists);
}

void ProviderHostScreen::startSelected()
{
    if (m_tunePending) {
        m_tuneFrames++;
        if (m_tuneFrames >= 10) {
            m_tunePending = false;
            m_resolving = true;
            if (evo_provider_resolve_chain(g_pending.provider, g_pending.item_id,
                                            on_resolved, &g_pending) != 0) {
                m_resolving = false;
                char why[256];
                const bool have = evo_failure_reason(why, sizeof why) != 0;
                toast("STREAM", have ? why : "That item could not be played");
                evo_rmlui_provider_set_tuning(0);
                evo_rmlui_provider_set_loading(0, "");
                evo_rmlui_provider_set_status(have ? why : "Failed to resolve stream for channel", 1);
            }
        }
        return;
    }

    evo_provider_selection_t sel;
    if (!evo_rmlui_provider_take_selection(&sel)) {
        /* Nothing was activated by hand. A Home resume waits here until its
         * provider is open (a sign-in may come first), then goes through the
         * exact path a pressed item takes. */
        if (!g_launchResume.active || !m_opened || m_resolving || g_start_running ||
            m_providerId != g_launchResume.provider)
            return;
        std::memset(&sel, 0, sizeof sel);
        std::snprintf(sel.provider_id, sizeof sel.provider_id, "%s", g_launchResume.provider.c_str());
        std::snprintf(sel.item_id,     sizeof sel.item_id,     "%s", g_launchResume.item_id.c_str());
        std::snprintf(sel.title,       sizeof sel.title,       "%s", g_launchResume.title.c_str());
        sel.is_live = g_launchResume.is_live ? 1 : 0;
        sel.resume_sec = g_launchResume.resume_sec;
        g_launchResume.active = false;
        g_launchResume.is_live = false;
    }
    if (m_resolving || g_start_running) return;

    /* A TV guide line that is not on now and not in the archive: say when. */
    {
        const char* in = inner_id(sel.item_id);
        if (std::strncmp(in, "gi:", 3) == 0 || std::strncmp(in, "xi:", 3) == 0) {
            const char* c1 = std::strchr(in + 3, ':');
            const long start = c1 ? std::atol(c1 + 1) : 0;
            char msg[80] = "That programme is not on now";
            if (std::strstr(in, ":later") && start > 0) {
                time_t t = static_cast<time_t>(start);
                struct tm tmv;
                localtime_r(&t, &tmv);
                std::snprintf(msg, sizeof msg, "Starts %02d-%02d at %02d:%02d", tmv.tm_mday, tmv.tm_mon + 1,
                              tmv.tm_hour, tmv.tm_min);
            } else if (std::strstr(in, ":past")) {
                std::snprintf(msg, sizeof msg, "Already shown - not in the archive");
            }
            evo_feedback(EVO_FB_BOUNDARY);
            toast("TV GUIDE", msg);
            return;
        }
    }

    std::memset(&g_pending, 0, sizeof g_pending);
    std::snprintf(g_pending.provider, sizeof g_pending.provider, "%s", sel.provider_id);
    std::snprintf(g_pending.item_id,  sizeof g_pending.item_id,  "%s", sel.item_id);
    std::snprintf(g_pending.title,    sizeof g_pending.title,    "%s", sel.title);
    g_pending.is_live = sel.is_live;
    g_pending.resume_sec = sel.resume_sec;
    g_resolve_finished = false;

    /* Read the setting now, not when the reply lands: what was on when the
     * channel was opened is what applies to it. */
    if (auto st = Application::getInstance().getSettingsService())
        g_pending.ask = st->isAskStreamEnabled() ? 1 : 0;
    if (const evo_provider_t* pp = evo_provider_find(sel.provider_id))
        if (pp->caps & EVO_PROVIDER_CAP_PICK) g_pending.ask = 1;
    g_pending.picked = 0;
    g_pick_stage = PickStage::Idle;
    ++g_pick_gen;
    g_variants = VariantsState::None;
    g_variants_merged = false;

    m_tunePending = true;
    m_tuneFrames = 0;

    char msg[128];
    const evo_provider_t* tp = evo_provider_find(sel.provider_id);
    const bool live = tp && (tp->caps & EVO_PROVIDER_CAP_LIVE);
    std::snprintf(msg, sizeof(msg), live ? "Tuning %s..." : "Opening %s...", sel.title);
    toast(live ? "LIVE TV" : (tp ? tp->name : "PROVIDER"), msg);
    evo_rmlui_provider_set_tuning(1);
    evo_rmlui_provider_set_loading(1, msg);
}

void ProviderHostScreen::update(double deltaMs)
{
    StatefulScreen::update(deltaMs);

    signInStep();
    stalkerStep();

    /* Home asked for a folder or a search: go there once the provider is open. */
    if (g_launchFolder.active && m_opened && !m_web && m_providerId == g_launchFolder.provider) {
        g_launchFolder.active = false;
        if (g_launchFolder.search) {
            m_searchQuery = g_launchFolder.id;
            evo_rmlui_provider_search(g_launchFolder.id.c_str());
        } else {
            evo_rmlui_provider_open_folder(g_launchFolder.id.c_str(), g_launchFolder.title.c_str());
        }
    }

    /* A new TV guide for the Stalker portal on screen: redraw the rows so
     * "now" and "next" appear without the user having to move. */
    if (m_providerId == "stalker") {
        static unsigned s_epgSeen = 0;
        const unsigned gen = stalker_epg_generation();
        if (gen != s_epgSeen) {
            s_epgSeen = gen;
            if (gen != 0 && !m_navigatingToPlayer) evo_rmlui_provider_reload();
        }
    }

    if (m_discovering) {
        char addr[128];
        int rc = evo_net_discover_poll(addr, sizeof addr);
        if (rc != 0) {
            m_discovering = false;
            const evo_provider_t* p = evo_provider_find(m_providerId.c_str());
            if (rc > 0) {
                evo_bt("prov_screen: discovery found %s for '%s'", addr, m_providerId.c_str());
                toast(p ? p->name : "PROVIDER", "Found a server - press Triangle to use it");
                openSourceKeyboard(addr);
            } else {
                evo_bt("prov_screen: discovery found nothing for '%s'", m_providerId.c_str());
                toast(p ? p->name : "PROVIDER", "No server found - type its address");
                openSourceKeyboard("http://");
            }
        }
    }

    /* #101: the web UI's session ended when the user closed the browser (a
     * handed-over stream keeps it alive). Back to the chooser, or out when
     * this is the only provider. */
    if (m_web) {
        /* EVO ended it (server down, page never loaded): say why, and stay
         * on the chooser so the address can be fixed right there. Checked
         * first: a refused connect can end the session before this screen
         * ever saw it running. */
        char err[256];
        if (!evo_webui_session_active() && evo_webui_take_error(err, sizeof err)) {
            m_web = false;
            m_webSeen = false;
            const evo_provider_t* p = evo_provider_find(m_providerId.c_str());
            toast(p ? p->name : "PROVIDER", err);
            enterPicker();
        } else if (evo_webui_session_active()) {
            m_webSeen = true;
        } else if (m_webSeen) {
            m_web = false;
            m_webSeen = false;
            if (pickable_count(nullptr) > 1) {
                enterPicker();
            } else if (auto sm = Application::getInstance().getScreenManager()) {
                sm->navigateTo(ScreenId::MainMenu);
            }
        }
        return;
    }
    if (!m_opened) return;

    /* The server rejected the saved session (expired, revoked, a server
     * reset): the catalog cannot load, so go to sign-in rather than leave
     * "Could not load the catalog" with no way forward. */
    if (const evo_provider_t* sp = evo_provider_find(m_providerId.c_str())) {
        if (sp->needs_sign_in && sp->sign_in && sp->needs_sign_in()) {
            evo_bt("prov_screen: '%s' session rejected - back to sign-in", m_providerId.c_str());
            toast(sp->name, "Please sign in again");
            evo_rmlui_provider_close();
            m_opened = false;
            beginSignIn();
            return;
        }
    }

    /* Bundle refresh, artwork decode, and any catalog reply that landed. */
    evo_rmlui_provider_tick();

    int action = evo_rmlui_provider_take_action();
    if (action == EVO_PROVIDER_ACTION_SETUP_URL) {
        openSourceEditor();
    } else if (action == EVO_PROVIDER_ACTION_SETUP_USB) {
        if (m_providerId == "stalker") {
            char msg[100] = {0};
            stalker_restore_categories(msg, sizeof msg);   /* the card is "restore categories" */
            toast("STALKER / MINISTRA", msg[0] ? msg : "Categories restored");
            evo_rmlui_provider_reload();
        } else {
            browseUsb();
        }
    } else if (action == EVO_PROVIDER_ACTION_CHOICE) {
        char choice[1100];
        if (evo_rmlui_provider_take_choice(choice, sizeof choice))
            applyChoice(choice);
    }

    if (m_providerId == "iptv") {
        char note[160];
        if (provider_iptv_take_guide_notice(note, sizeof note))
            toast("LIVE TV", (std::string(note) + " · change in Options").c_str());
    }

    if (g_start_running && g_start_done) {
        pthread_join(g_start_thread, nullptr);
        g_start_running = false;
        m_resolving = false;
        g_resolve_finished = false;

        if (g_start_success) {
            evo_rmlui_provider_set_tuning(0);
            evo_rmlui_provider_set_loading(0, "");
            setNavigatingToPlayer(true);
            if (auto sm = Application::getInstance().getScreenManager()) {
                sm->navigateTo(ScreenId::Player);
            }
        } else {
            /* The address was a playlist, not a stream (an IPTV link given where
             * a channel belongs): load it as the channel list it is. */
            char plurl[2048];
            if (std::strcmp(g_pending.provider, "iptv") == 0 &&
                evo_stream_io_take_channel_playlist(plurl, sizeof plurl) &&
                adoptChannelPlaylist(plurl)) {
                return;
            }
            char why[256];
            const bool have_why = evo_failure_reason(why, sizeof why) != 0;

            if (g_pending.picked) {
                /* The user chose this stream. Nothing else is tried for them:
                 * say what happened and give the list back. */
                const evo_stream_choice_t& bad = g_pending.choices[g_pending.active_choice];
                char msg[256];
                if (have_why)
                    std::snprintf(msg, sizeof(msg), "%s did not open: %s", bad.label, why);
                else
                    std::snprintf(msg, sizeof(msg), "%s did not open - pick another", bad.label);
                toast("STREAM", msg);
                evo_bt("prov_screen: chosen stream %d failed (%s) - back to the list",
                       g_pending.active_choice, bad.url);
                evo_rmlui_provider_set_tuning(0);
                evo_rmlui_provider_set_loading(0, "");
                m_resolving = false;
                m_streamPick = true;
                m_streamIndex = g_pending.active_choice;
                return;
            }
            if (g_pending.active_choice + 1 < g_pending.choice_count) {
                g_pending.active_choice++;
                evo_bt("prov_screen: stream choice %d failed, falling back to choice %d: %s",
                       g_pending.active_choice - 1, g_pending.active_choice,
                       g_pending.choices[g_pending.active_choice].url);
                char retry_msg[128];
                std::snprintf(retry_msg, sizeof(retry_msg), "Connecting to backup (%s)...",
                              g_pending.choices[g_pending.active_choice].label);
                toast("STREAM", retry_msg);
                evo_rmlui_provider_set_status(retry_msg, 0);

                launch_choice(g_pending.active_choice);
                return;
            }
            evo_rmlui_provider_set_tuning(0);
            evo_rmlui_provider_set_loading(0, "");
            evo_rmlui_provider_set_status(have_why ? why
                                          : "Stream unavailable or connection timed out", 1);
        }
    }

    /* The qualities have landed while the picker is open: the rows after the listed
     * one just moved down, so the cursor moves with them. */
    if (g_variants_merged) {
        g_variants_merged = false;
        if (m_streamPick && m_streamIndex > 0)
            m_streamIndex += g_variants_added;
        g_variants_added = 0;
    }

    /* The read is bounded in what it claims: after a while the list stops saying
     * "reading" and a late reply is dropped. */
    if (g_variants == VariantsState::Reading) {
        const auto waited = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - g_variants_started).count();
        if (waited > kVariantsGiveUpMs) {
            ++g_pick_gen;
            g_variants = VariantsState::Failed;
            evo_bt("provider: stream picker: the master did not answer in %d ms", kVariantsGiveUpMs);
        }
    }

    /* One row and no qualities coming is not a choice: play it. (Once - if the
     * user's own pick fails the list returns and stays.) */
    if (m_streamPick && g_variants == VariantsState::Failed && g_pending.choice_count == 1 &&
        !g_pending.picked && !g_start_running) {
        chooseStream(0);
    }

    if (g_pick_stage == PickStage::Ready && !m_streamPick)
        enterStreamPicker();

    /*
     * Take the activated item here rather than inside the Rml event handler
     * that produced it. Starting playback tears down and rebuilds the VideoOut
     * configuration, and doing that from underneath Context::Update() - which
     * is still walking the element tree that raised the event - is the shape of
     * bug that cost two console cycles in #32.
     */
    startSelected();

    /* A resolve that finished - either way - releases the guard, so a failed
     * one does not leave the screen permanently unable to activate anything. */
    if (m_resolving && g_resolve_finished) {
        m_resolving = false;
        g_resolve_finished = false;
    }
}

void ProviderHostScreen::render(uint32_t* framebuffer, int width, int height)
{
    if (m_actionMenu) {
        renderActionMenu(framebuffer, width, height);
        return;
    }
    if (m_streamPick) {
        renderStreamPicker(framebuffer, width, height);
        return;
    }
    if (g_grid.open && m_opened && !m_picking && !m_web) {
        renderGrid(framebuffer, width, height);
        return;
    }
    if (m_picking || m_web) {
        renderPicker(framebuffer, width, height);
        return;
    }
    if (!m_opened) return;
    const bool kit_drew = evo_rmlui_provider_render(framebuffer, width, height) != 0;
    /* The rail is a document in the MAIN context, so it does not come with the
     * provider's own context - it is composited on top afterwards. The kit draws
     * its own rail with the screen. */
    if (!kit_drew)
        evo_rmlui_render_nav_overlay(framebuffer, width, height);
    evo_rmlui_provider_clear_frame();
}

/* ------------------------------------------------------------------------- */
/* Media-server sign-in                                                      */
/* ------------------------------------------------------------------------- */

bool ProviderHostScreen::isNativeWeb(const ::evo_provider* p)
{
    return p && (p->caps & EVO_PROVIDER_CAP_WEBUI) &&
           (p->caps & EVO_PROVIDER_CAP_CATALOG) && p->list_catalog;
}

void ProviderHostScreen::beginSignIn()
{
    const evo_provider_t* p = evo_provider_find(m_providerId.c_str());
    if (!p || !p->sign_in) return;
    std::string id = m_providerId;
    enterPicker();                  /* the chooser sits behind the keyboards */
    m_providerId = id;
    m_signInUser.clear();
    m_signInPass.clear();
    m_qcCode.clear();
    evo_bt("prov_screen: sign-in to '%s' begins", id.c_str());
    /* Quick Connect first where the server has it: approving a six-digit code
     * on a phone beats spelling a password with the D-pad. A server with it
     * switched off answers the start with an error and falls through. */
    if (p->qc_start) {
        m_signIn = SignIn::QcStart;
        if (p->qc_start(&ProviderHostScreen::OnQcCode, this) == 0) return;
    }
    beginPasswordSignIn();
}

void ProviderHostScreen::beginPasswordSignIn()
{
    const evo_provider_t* p = evo_provider_find(m_providerId.c_str());
    if (!p || !p->sign_in) { m_signIn = SignIn::None; return; }
    m_signIn = SignIn::Suggest;
    /* The server's first public user pre-fills the name - one press, not a
     * spelled-out user name. A server that hides its users just gives "". */
    if (!p->suggest_user || p->suggest_user(&ProviderHostScreen::OnSignInSuggest, this) != 0)
        m_signIn = SignIn::OpenUser;
}

static long long qc_now_ms()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

void ProviderHostScreen::OnQcCode(int ok, const char* code, void* ud)
{
    auto* self = static_cast<ProviderHostScreen*>(ud);
    if (!self || self->m_signIn != SignIn::QcStart) return;
    if (!ok || !code || !code[0]) {
        evo_bt("prov_screen: quick connect unavailable for '%s' - password sign-in",
               self->m_providerId.c_str());
        self->beginPasswordSignIn();
        return;
    }
    self->m_qcCode = code;
    self->m_qcNextPollMs = qc_now_ms() + 3000;
    /* Jellyfin drops a pending request after about ten minutes. */
    self->m_qcDeadlineMs = qc_now_ms() + 9 * 60 * 1000;
    self->m_signIn = SignIn::QcWait;
    evo_bt("prov_screen: quick connect code %s for '%s'", code, self->m_providerId.c_str());
}

void ProviderHostScreen::OnQcPoll(int state, const char* msg, void* ud)
{
    auto* self = static_cast<ProviderHostScreen*>(ud);
    if (!self || self->m_signIn != SignIn::QcPolling) return;
    const evo_provider_t* p = evo_provider_find(self->m_providerId.c_str());
    if (state == 0) {
        self->m_qcNextPollMs = qc_now_ms() + 3000;
        self->m_signIn = SignIn::QcWait;
        return;
    }
    self->m_signIn = SignIn::None;
    self->m_qcCode.clear();
    evo_bt("prov_screen: quick connect for '%s' -> %s (%s)", self->m_providerId.c_str(),
           state > 0 ? "signed in" : "failed", msg ? msg : "");
    toast(p ? p->name : "PROVIDER", msg && msg[0] ? msg : (state > 0 ? "Signed in" : "Quick Connect failed"));
    if (state > 0)
        self->openProvider(self->m_providerId);
}

/* While the code is on screen: Square switches to the password, Circle gives
 * up. Everything else is swallowed - there is nothing else to press. */
bool ProviderHostScreen::signInQcInput(uint32_t pressed)
{
    if (m_signIn != SignIn::QcStart && m_signIn != SignIn::QcWait &&
        m_signIn != SignIn::QcPolling)
        return false;
    if (pressed & PadButtons::Square) {
        evo_feedback(EVO_FB_OPEN);
        m_qcCode.clear();
        beginPasswordSignIn();
    } else if (pressed & PadButtons::Circle) {
        evo_feedback(EVO_FB_CANCEL);
        m_qcCode.clear();
        m_signIn = SignIn::None;
        evo_bt("prov_screen: quick connect for '%s' cancelled", m_providerId.c_str());
    }
    return true;
}

void ProviderHostScreen::renderQuickConnect(uint32_t* framebuffer, int width, int height)
{
    evo_rmlui_list_params_t params;
    std::memset(&params, 0, sizeof(params));
    params.section = EVO_SECTION_EMBY;
    const evo_provider_t* p = evo_provider_find(m_providerId.c_str());
    static char title[64], hint[160], code[16];
    std::snprintf(title, sizeof title, "SIGN IN TO %s", p ? p->name : "THE SERVER");
    params.title = title;
    params.subtitle = "Quick Connect - no password needed";
    params.is_empty = 1;
    if (m_qcCode.size() == 6)
        std::snprintf(code, sizeof code, "%.3s %.3s", m_qcCode.c_str(), m_qcCode.c_str() + 3);
    else
        std::snprintf(code, sizeof code, "%s", m_qcCode.empty() ? "..." : m_qcCode.c_str());
    params.empty_title = code;
    std::snprintf(hint, sizeof hint,
                  "In %s on your phone or computer, open Settings > Quick Connect and enter this code.",
                  p ? p->name : "the app");
    params.empty_hint = hint;
    params.empty_icon = "../icons/icon_emby.png";
    params.hint_count = 2;
    params.hints[0].glyph_path = "../icons/btn_square.png";
    params.hints[0].label = "USE PASSWORD";
    params.hints[1].glyph_path = "../icons/btn_circle.png";
    params.hints[1].label = "CANCEL";
    evo_rmlui_update_list(&params);
    evo_rmlui_render_list(framebuffer, width, height);
}

void ProviderHostScreen::OnSignInSuggest(const char* name, void* ud)
{
    auto* self = static_cast<ProviderHostScreen*>(ud);
    if (!self || self->m_signIn != SignIn::Suggest) return;
    self->m_signInUser = name ? name : "";
    self->m_signIn = SignIn::OpenUser;
}

void ProviderHostScreen::OnSignInUser(const char* text, void* ud)
{
    auto* self = static_cast<ProviderHostScreen*>(ud);
    if (!self) return;
    self->m_signInUser = text ? text : "";
    self->m_signIn = self->m_signInUser.empty() ? SignIn::None : SignIn::OpenPass;
}

void ProviderHostScreen::OnSignInPass(const char* text, void* ud)
{
    auto* self = static_cast<ProviderHostScreen*>(ud);
    if (!self) return;
    self->m_signInPass = text ? text : "";
    const evo_provider_t* p = evo_provider_find(self->m_providerId.c_str());
    self->m_signIn = SignIn::Signing;
    int rc = (p && p->sign_in)
           ? p->sign_in(self->m_signInUser.c_str(), self->m_signInPass.c_str(),
                        &ProviderHostScreen::OnSignInDone, self)
           : -1;
    self->m_signInPass.assign(self->m_signInPass.size(), '\0');
    self->m_signInPass.clear();
    if (rc != 0) {
        self->m_signIn = SignIn::None;
        char why[256];
        toast(p ? p->name : "PROVIDER",
              evo_failure_reason(why, sizeof why) ? why : "Could not reach the server");
    }
}

void ProviderHostScreen::OnSignInDone(int ok, const char* msg, void* ud)
{
    auto* self = static_cast<ProviderHostScreen*>(ud);
    if (!self || self->m_signIn != SignIn::Signing) return;
    self->m_signIn = SignIn::None;
    const evo_provider_t* p = evo_provider_find(self->m_providerId.c_str());
    evo_bt("prov_screen: sign-in to '%s' -> %s (%s)", self->m_providerId.c_str(),
           ok ? "ok" : "failed", msg ? msg : "");
    char why[256];
    const char* shown = msg && msg[0] ? msg : nullptr;
    if (!shown && !ok && evo_failure_reason(why, sizeof why)) shown = why;
    toast(p ? p->name : "PROVIDER", shown ? shown : (ok ? "Signed in" : "Sign-in failed"));
    if (ok)
        self->openProvider(self->m_providerId);
}

/*
 * The address behind a channel was a playlist, not a stream. Make it the IPTV
 * source and reopen the channel list. False if the provider will not take it,
 * in which case the caller reports the failure as usual.
 */
bool ProviderHostScreen::adoptChannelPlaylist(const char* url)
{
    const evo_provider_t* ip = evo_provider_find("iptv");
    if (!ip || !ip->set_source || ip->set_source(url) != 0)
        return false;
    evo_provider_set_enabled(ip->id, ip->is_configured() ? 1 : 0);
    evo_bt("prov_screen: the channel address is a playlist - loaded as the IPTV source");
    toast("IPTV", "That link is a playlist - loading it as your channel list");

    evo_rmlui_provider_set_tuning(0);
    evo_rmlui_provider_set_loading(0, "");
    m_resolving = false;
    m_streamPick = false;
    std::string id = m_providerId;
    if (m_opened) {
        evo_rmlui_provider_close();
        m_opened = false;
    }
    m_opened = evo_rmlui_provider_open(id.c_str(), DisplayWidth, DisplayHeight) != 0;
    return true;
}

/* Called every frame from update(). Opens the next keyboard, and notices a
 * keyboard the user cancelled (Circle closes it without a callback). */
void ProviderHostScreen::signInStep()
{
    const evo_provider_t* p = evo_provider_find(m_providerId.c_str());
    const char* name = p ? p->name : "Server";
    char title[96];
    switch (m_signIn) {
    case SignIn::QcWait: {
        const long long now = qc_now_ms();
        if (now > m_qcDeadlineMs) {
            m_signIn = SignIn::None;
            m_qcCode.clear();
            toast(name, "The code expired - press X to get a new one");
            break;
        }
        if (now >= m_qcNextPollMs && p && p->qc_poll) {
            m_signIn = SignIn::QcPolling;
            if (p->qc_poll(&ProviderHostScreen::OnQcPoll, this) != 0) {
                m_signIn = SignIn::QcWait;
                m_qcNextPollMs = now + 3000;
            }
        }
        break;
    }
    case SignIn::OpenUser:
        std::snprintf(title, sizeof title, "%s user name", name);
        m_signIn = SignIn::User;
        evo_keyboard_open(title, m_signInUser.c_str(), 63,
                          &ProviderHostScreen::OnSignInUser, this);
        break;
    case SignIn::OpenPass:
        std::snprintf(title, sizeof title, "Password for %s (empty if none)", m_signInUser.c_str());
        m_signIn = SignIn::Pass;
        evo_keyboard_open(title, "", 63, &ProviderHostScreen::OnSignInPass, this);
        break;
    case SignIn::User:
    case SignIn::Pass:
        if (!evo_keyboard_is_open()) {
            evo_bt("prov_screen: sign-in to '%s' cancelled", m_providerId.c_str());
            m_signIn = SignIn::None;
        }
        break;
    default:
        break;
    }
}

/* Stalker / Ministra: the second keyboard, for the MAC address. */
void ProviderHostScreen::OnStalkerMac(const char* text, void* userdata)
{
    auto* self = static_cast<ProviderHostScreen*>(userdata);
    if (!self) return;
    self->m_stalkerStep = StalkerStep::None;

    std::string mac = text ? text : "";
    size_t b = mac.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) {
        toast("Stalker / Ministra", "A MAC address is needed");
        return;
    }
    size_t e = mac.find_last_not_of(" \t\r\n");
    mac = mac.substr(b, e - b + 1);

    /* "<url> <mac>" goes through the normal source path (persist, enable,
     * reopen). It has a space in it, so it is not asked for a MAC again. */
    const std::string both = self->m_stalkerUrl + " " + mac;
    OnSourceSubmitted(both.c_str(), self);
}

/* R3 on a category or channel: ask for the place it should have, then put it there. */
void ProviderHostScreen::stalkerMovePrompt()
{
    const char* id = evo_rmlui_provider_get_focused_id();
    int pos = 0, total = 0;
    if (!id || stalker_move_info(id, &pos, &total) != 0) {
        toast("STALKER / MINISTRA", "Pick a category or a channel first");
        return;
    }
    m_moveId = id;
    char title[96];
    std::snprintf(title, sizeof title, "Move to position (1-%d)", total);
    const std::string initial = std::to_string(pos);
    evo_keyboard_open_phone(title, initial.c_str(), 8, &ProviderHostScreen::OnStalkerMove, this);
}

void ProviderHostScreen::OnStalkerMove(const char* text, void* userdata)
{
    auto* self = static_cast<ProviderHostScreen*>(userdata);
    if (!self) return;
    const int n = text ? std::atoi(text) : 0;
    if (n < 1) {
        toast("STALKER / MINISTRA", "Type a position, for example 1");
        return;
    }
    char msg[120] = {0};
    const int r = stalker_move_to(self->m_moveId.c_str(), n, msg, sizeof msg);
    if (msg[0]) toast("STALKER / MINISTRA", msg);
    if (r == 0) {
        evo_feedback(EVO_FB_CONFIRM);
        evo_rmlui_provider_reload();
    }
}

/* ------------------------------------------------------------------------- */
/* The TV guide grid                                                         */
/* ------------------------------------------------------------------------- */
/*
 * Opened on a channel list (touchpad, or Options > TV guide grid): the list's
 * channels down the side, their programmes along a two-hour clock from the
 * TV guide (the provider's, or the one added in Settings). X on what is on
 * now watches the channel, on a past programme with catch-up plays it back.
 */
namespace {

constexpr long long kGridSpan = 7200;          /* two hours across */
constexpr int kGridRows = EVO_HUI_GUIDE_ROWS;

long long grid_now()
{
    return static_cast<long long>(std::time(nullptr));
}

int grid_utc_offset(long long t)
{
    time_t tt = static_cast<time_t>(t);
    struct tm lt;
    if (!localtime_r(&tt, &lt)) return 0;
    return static_cast<int>(lt.tm_gmtoff);
}

std::string grid_clock(long long t)
{
    time_t tt = static_cast<time_t>(t);
    struct tm lt;
    localtime_r(&tt, &lt);
    char b[16];
    std::snprintf(b, sizeof b, "%02d:%02d", lt.tm_hour, lt.tm_min);
    return b;
}

void grid_progs(const GridChan& c, long long from, long long to, std::vector<GridProg>* out)
{
    out->clear();
    evo_epg_prog_t buf[96];
    const int n = evo_epg_programmes(g_grid.provider.c_str(), c.epg.c_str(), c.name.c_str(),
                                     from, to, buf, 96);
    for (int i = 0; i < n; ++i) {
        GridProg g;
        g.a = buf[i].start;
        g.b = buf[i].stop;
        g.title = buf[i].title ? buf[i].title : "";
        g.desc = buf[i].desc ? buf[i].desc : "";
        out->push_back(std::move(g));
    }
}

int grid_find(const std::vector<GridProg>& p, long long t)
{
    for (size_t i = 0; i < p.size(); ++i)
        if (p[i].a <= t && t < p[i].b) return static_cast<int>(i);
    return -1;
}

/* The id that plays programme g of channel c again, "" when there is none. */
std::string grid_catchup_id(const GridChan& c, const GridProg& g)
{
    const long long now = grid_now();
    if (g.b > now || g.a <= 0) return std::string();
    if (g_grid.provider == "iptv") {
        char id[EVO_PROVIDER_MAX_ITEM_ID];
        if (provider_iptv_catchup_id(c.id.c_str(), g.a, g.b, id, sizeof id) == 0) return id;
        return std::string();
    }
    if (g_grid.provider == "xtream" && c.days > 0 && g.a >= now - static_cast<long long>(c.days) * 86400) {
        const size_t ls = c.id.find("live_stream:");
        if (ls == std::string::npos) return std::string();
        return c.id.substr(0, ls) + "xa:" + c.id.substr(ls + 12) + ":" + std::to_string(static_cast<long>(g.a)) + ":" +
               std::to_string(static_cast<long>(g.b));
    }
    return std::string();
}

void grid_follow()
{
    /* The focused channel on screen, and the cursor inside the window. */
    const int n = static_cast<int>(g_grid.ch.size());
    if (g_grid.sel < 0) g_grid.sel = 0;
    if (g_grid.sel >= n) g_grid.sel = n - 1;
    if (g_grid.sel < g_grid.top) g_grid.top = g_grid.sel;
    if (g_grid.sel >= g_grid.top + kGridRows) g_grid.top = g_grid.sel - kGridRows + 1;
    while (g_grid.cursor < g_grid.win) g_grid.win -= 1800;
    while (g_grid.cursor >= g_grid.win + kGridSpan - 900) g_grid.win += 1800;
}
} // namespace

void ProviderHostScreen::openGrid()
{
    EvoRmlProviderHost& host = EvoRmlProviderHost::Instance();
    if (host.SearchShown()) {
        toast("TV GUIDE", "Open a channel list first - not search results");
        return;
    }
    const std::vector<EvoProviderRow>& rows = host.AllRows();
    const int focused = host.FocusedAbsIndex();
    g_grid = GridState();
    g_grid.provider = m_providerId;
    for (size_t i = 0; i < rows.size(); ++i) {
        const EvoProviderRow& r = rows[i];
        if (r.is_folder || !is_channel_id(inner_id(r.id.c_str()))) continue;
        GridChan c;
        c.id = r.id.c_str();
        c.name = r.chan_key.empty() ? std::string(r.title.c_str()) : std::string(r.chan_key.c_str());
        c.abs = i;
        char epg[128] = "";
        if (m_providerId == "iptv") {
            char name[EVO_PROVIDER_MAX_TITLE] = "";
            if (provider_iptv_channel_info(c.id.c_str(), epg, sizeof epg, name, sizeof name, &c.days) == 0 &&
                name[0])
                c.name = name;
        } else if (m_providerId == "xtream") {
            xtream_channel_info(c.id.c_str(), epg, sizeof epg, &c.days);
        }
        c.epg = epg;
        if (static_cast<int>(i) == focused) g_grid.sel = static_cast<int>(g_grid.ch.size());
        g_grid.ch.push_back(std::move(c));
    }
    if (g_grid.ch.empty()) {
        toast("TV GUIDE", "Open a list of channels first");
        return;
    }
    const long long now = grid_now();
    g_grid.cursor = now;
    g_grid.win = (now / 1800) * 1800 - 1800;
    g_grid.top = g_grid.sel - kGridRows / 2;
    if (g_grid.top + kGridRows > static_cast<int>(g_grid.ch.size()))
        g_grid.top = static_cast<int>(g_grid.ch.size()) - kGridRows;
    if (g_grid.top < 0) g_grid.top = 0;
    g_grid.open = true;
    grid_follow();
    evo_feedback(EVO_FB_OPEN);
}

void ProviderHostScreen::gridStartFocused()
{
    const GridChan& c = g_grid.ch[static_cast<size_t>(g_grid.sel)];
    std::vector<GridProg> p;
    grid_progs(c, g_grid.cursor - 86400, g_grid.cursor + 86400, &p);
    const int at = grid_find(p, g_grid.cursor);
    const long long now = grid_now();
    std::string id;
    std::string title = c.name;
    bool live = false;
    if (at < 0 || (p[(size_t)at].a <= now && now < p[(size_t)at].b)) {
        id = c.id;                                   /* on now, or no guide: the channel */
        live = true;
    } else if (p[(size_t)at].b <= now) {
        id = grid_catchup_id(c, p[(size_t)at]);
        if (id.empty()) {
            evo_feedback(EVO_FB_BOUNDARY);
            toast("TV GUIDE", g_grid.provider == "stalker"
                                  ? "Catch-up for this channel is in its TV guide (Options)"
                                  : "Already shown - not in the archive");
            return;
        }
        title = c.name + " - " + p[(size_t)at].title;
    } else {
        evo_feedback(EVO_FB_BOUNDARY);
        toast("TV GUIDE", ("Starts at " + grid_clock(p[(size_t)at].a)).c_str());
        return;
    }
    if (m_resolving || m_tunePending || g_start_running) return;
    g_grid.open = false;
    EvoRmlProviderHost::Instance().FocusAbs(c.abs);
    g_launchResume.active = true;
    g_launchResume.provider = m_providerId;
    g_launchResume.item_id = id;
    g_launchResume.title = title;
    g_launchResume.resume_sec = 0;
    g_launchResume.is_live = live;
    evo_feedback(EVO_FB_OPEN);
}

bool ProviderHostScreen::gridInput(uint32_t pressed)
{
    const int n = static_cast<int>(g_grid.ch.size());
    if (pressed & (PadButtons::Circle | PadButtons::TouchPad)) {
        evo_feedback(EVO_FB_CANCEL);
        g_grid.open = false;
        if (g_grid.sel >= 0 && g_grid.sel < n)
            EvoRmlProviderHost::Instance().FocusAbs(g_grid.ch[(size_t)g_grid.sel].abs);
        return true;
    }
    if (pressed & PadButtons::Cross) {
        gridStartFocused();
        return true;
    }
    if (pressed & (PadButtons::Up | PadButtons::Down | PadButtons::L1 | PadButtons::R1)) {
        const int step = (pressed & (PadButtons::L1 | PadButtons::R1)) ? kGridRows : 1;
        const int dir = (pressed & (PadButtons::Up | PadButtons::L1)) ? -1 : 1;
        const int to = std::max(0, std::min(n - 1, g_grid.sel + dir * step));
        if (to == g_grid.sel) { evo_feedback(EVO_FB_BOUNDARY); return true; }
        g_grid.sel = to;
        if (step > 1) g_grid.top = std::max(0, std::min(std::max(0, n - kGridRows), g_grid.top + dir * step));
        evo_feedback(EVO_FB_MOVE);
        grid_follow();
        return true;
    }
    if (pressed & (PadButtons::Left | PadButtons::Right)) {
        const bool left = (pressed & PadButtons::Left) != 0;
        std::vector<GridProg> p;
        grid_progs(g_grid.ch[(size_t)g_grid.sel], g_grid.cursor - 86400, g_grid.cursor + 86400, &p);
        const int at = grid_find(p, g_grid.cursor);
        long long to = g_grid.cursor + (left ? -1800 : 1800);
        if (at >= 0) {
            if (left && at > 0)                         to = p[(size_t)at - 1].a;
            else if (left)                              to = p[(size_t)at].a - 1;
            else if (at + 1 < (int)p.size())            to = p[(size_t)at + 1].a;
            else                                        to = p[(size_t)at].b;
        } else if (!p.empty()) {
            /* Between programmes: the nearest one that way. */
            for (size_t i = 0; i < p.size(); ++i) {
                if (!left && p[i].a > g_grid.cursor) { to = p[i].a; break; }
                if (left && p[i].b <= g_grid.cursor) to = p[i].a;
            }
        }
        const long long now = grid_now();
        if (to < now - 8 * 86400 || to > now + 8 * 86400) { evo_feedback(EVO_FB_BOUNDARY); return true; }
        /* Keep the cursor visible at the window's edge rather than at a
         * programme's start hours away. */
        if (left && to < g_grid.win && g_grid.cursor > g_grid.win) to = std::max(to, g_grid.win);
        g_grid.cursor = to;
        evo_feedback(EVO_FB_MOVE);
        grid_follow();
        return true;
    }
    if (pressed & (PadButtons::L2 | PadButtons::R2)) {
        const long long d = (pressed & PadButtons::L2) ? -3600 : 3600;
        const long long now = grid_now();
        if (g_grid.win + d < now - 8 * 86400 || g_grid.win + d > now + 8 * 86400) {
            evo_feedback(EVO_FB_BOUNDARY);
            return true;
        }
        g_grid.win += d;
        g_grid.cursor += d;
        evo_feedback(EVO_FB_MOVE);
        grid_follow();
        return true;
    }
    if (pressed & PadButtons::Options) {
        evo_feedback(EVO_FB_CANCEL);
        g_grid.open = false;
        return true;
    }
    return true;                                     /* the grid has the pad */
}

void ProviderHostScreen::renderGrid(uint32_t* framebuffer, int width, int height)
{
    const long long now = grid_now();
    const int n = static_cast<int>(g_grid.ch.size());
    static evo_hui_guide_params_t gp;
    std::memset(&gp, 0, sizeof gp);
    std::vector<std::vector<GridProg>> progs((size_t)kGridRows);
    std::vector<std::string> numbers((size_t)kGridRows);
    std::vector<std::string> notes;
    const long long win_end = g_grid.win + kGridSpan;
    gp.title = "TV GUIDE";
    static std::string crumb;
    crumb = g_grid.ch[(size_t)g_grid.sel].name;
    gp.crumb = crumb.c_str();
    gp.now = now;
    gp.win_start = g_grid.win;
    gp.win_end = win_end;
    gp.utc_offset = grid_utc_offset(now);
    gp.first = g_grid.top;
    gp.total = n;
    int with_guide = 0;
    for (int r = 0; r < kGridRows && g_grid.top + r < n; ++r) {
        const GridChan& c = g_grid.ch[(size_t)(g_grid.top + r)];
        evo_hui_guide_row_t& row = gp.rows[r];
        grid_progs(c, g_grid.win - 3 * 3600, win_end + 3 * 3600, &progs[(size_t)r]);
        numbers[(size_t)r] = std::to_string(g_grid.top + r + 1);
        row.name = c.name.c_str();
        row.number = numbers[(size_t)r].c_str();
        row.focused = (g_grid.top + r == g_grid.sel);
        row.archive = c.days > 0 ? 1 : 0;
        row.focus_prog = -1;
        int k = 0;
        for (const GridProg& g : progs[(size_t)r]) {
            if (g.b <= g.a || g.b <= g_grid.win || g.a >= win_end) continue;
            if (k >= EVO_HUI_GUIDE_PROGS) break;
            evo_hui_guide_prog_t& o = row.progs[k];
            o.start = g.a;
            o.stop = g.b;
            o.title = g.title.c_str();
            o.catchup = (g.b <= now && !grid_catchup_id(c, g).empty()) ? 1 : 0;
            if (row.focused && g.a <= g_grid.cursor && g_grid.cursor < g.b) row.focus_prog = k;
            ++k;
        }
        row.prog_count = k;
        if (k) ++with_guide;
        gp.row_count = r + 1;
    }

    /* The focused programme. */
    static std::string sel_title, sel_time, sel_state, sel_desc;
    sel_title.clear(); sel_time.clear(); sel_state.clear(); sel_desc.clear();
    const GridChan& fc = g_grid.ch[(size_t)g_grid.sel];
    std::vector<GridProg> fp;
    grid_progs(fc, g_grid.cursor - 86400, g_grid.cursor + 86400, &fp);
    const int at = grid_find(fp, g_grid.cursor);
    gp.hint_accept = "WATCH";
    if (at >= 0) {
        const GridProg& g = fp[(size_t)at];
        sel_title = g.title;
        sel_desc = g.desc;
        sel_time = grid_clock(g.a) + " - " + grid_clock(g.b);
        if (g.a <= now && now < g.b) {
            sel_state = "On now";
        } else if (g.b <= now) {
            if (!grid_catchup_id(fc, g).empty()) sel_state = "Catch-up";
            else { sel_state = "Already shown"; gp.hint_accept = ""; }
        } else {
            sel_state = "Starts at " + grid_clock(g.a);
            gp.hint_accept = "";
        }
    }
    gp.sel_title = sel_title.c_str();
    gp.sel_time = sel_time.c_str();
    gp.sel_state = sel_state.c_str();
    gp.sel_desc = sel_desc.c_str();
    gp.note = with_guide ? "" : (g_grid.provider == "stalker"
        ? "No full TV guide for these channels. Add one in Settings - TV guide."
        : "No TV guide for these channels yet. It may still be loading, or add one in Options - Playlist & guide / Settings - TV guide.");

    if (evo_rmlui_render_guide(&gp, width, height)) return;

    /* No kit: the focused channel's programmes as a list. */
    evo_rmlui_list_params_t lp;
    std::memset(&lp, 0, sizeof lp);
    lp.section = EVO_SECTION_EMBY;
    lp.title = "TV GUIDE";
    lp.subtitle = crumb.c_str();
    std::vector<std::string> titles, details;
    int first = at > 2 ? at - 2 : 0;
    const int count = std::min(EVO_RMLUI_LIST_ROWS, static_cast<int>(fp.size()) - first);
    titles.resize((size_t)std::max(0, count));
    details.resize((size_t)std::max(0, count));
    for (int i = 0; i < count; ++i) {
        const GridProg& g = fp[(size_t)(first + i)];
        titles[(size_t)i] = grid_clock(g.a) + "  " + g.title;
        const bool on = g.a <= now && now < g.b;
        details[(size_t)i] = on ? "On now" : (g.b <= now ? (grid_catchup_id(fc, g).empty() ? "Already shown" : "Catch-up")
                                                        : "Later");
        lp.rows[i].title = titles[(size_t)i].c_str();
        lp.rows[i].detail = details[(size_t)i].c_str();
        lp.rows[i].icon_path = "../icons/icon_tv.png";
        lp.rows[i].badge = "";
        lp.rows[i].progress = -1;
        lp.rows[i].is_focused = (first + i == at);
    }
    lp.row_count = std::max(0, count);
    lp.total_count = static_cast<int>(fp.size());
    lp.cursor_index = at;
    lp.is_empty = fp.empty() ? 1 : 0;
    lp.empty_title = "No programme information";
    lp.empty_hint = gp.note;
    lp.empty_icon = "../icons/icon_tv.png";
    lp.hint_count = 2;
    lp.hints[0].glyph_path = "../icons/btn_cross.png";
    lp.hints[0].label = "WATCH";
    lp.hints[1].glyph_path = "../icons/btn_circle.png";
    lp.hints[1].label = "CLOSE";
    evo_rmlui_update_list(&lp);
    evo_rmlui_render_list(framebuffer, width, height);
}

/* ---- the viewer's arrangement of channels (evo_chanprefs) ---------------- */

/* Moves the focused channel by `n` places (rel) or to place `n` (0-based). */
void ProviderHostScreen::chanMove(int n, bool rel)
{
    EvoRmlProviderHost& host = EvoRmlProviderHost::Instance();
    const std::vector<EvoProviderRow>& rows = host.AllRows();
    const int at = host.FocusedAbsIndex();
    if (host.SearchShown() || at < 0 || rows[(size_t)at].chan_key.empty()) {
        toast("CHANNELS", "Pick a channel in a list first");
        return;
    }
    std::vector<const char*> names;
    for (const EvoProviderRow& r : rows) {
        if (r.chan_key.empty()) { toast("CHANNELS", "This list cannot be arranged"); return; }
        names.push_back(r.chan_key.c_str());
    }
    int to = rel ? at + n : n;
    if (to < 0) to = 0;
    if (to >= (int)names.size()) to = (int)names.size() - 1;
    if (to == at) { evo_feedback(EVO_FB_BOUNDARY); return; }
    evo_chanprefs_move(m_providerId.c_str(), host.CurrentParent().c_str(), names.data(), (int)names.size(), at, to);
    evo_feedback(EVO_FB_CONFIRM);
    host.ReloadFocusAt((size_t)to);
}

void ProviderHostScreen::chanMovePrompt()
{
    EvoRmlProviderHost& host = EvoRmlProviderHost::Instance();
    const int at = host.FocusedAbsIndex();
    const int total = (int)host.AllRows().size();
    if (at < 0 || focused_chan_key().empty()) {
        toast("CHANNELS", "Pick a channel in a list first");
        return;
    }
    char title[96];
    std::snprintf(title, sizeof title, "Move to position (1-%d)", total);
    const std::string initial = std::to_string(at + 1);
    evo_keyboard_open_phone(title, initial.c_str(), 8, &ProviderHostScreen::OnChanMove, this);
}

void ProviderHostScreen::OnChanMove(const char* text, void* userdata)
{
    auto* self = static_cast<ProviderHostScreen*>(userdata);
    if (!self) return;
    const int n = text ? std::atoi(text) : 0;
    if (n < 1) {
        toast("CHANNELS", "Type a position, for example 1");
        return;
    }
    self->chanMove(n - 1, false);
}

void ProviderHostScreen::chanHide()
{
    EvoRmlProviderHost& host = EvoRmlProviderHost::Instance();
    const std::string key = focused_chan_key();
    const int at = host.FocusedAbsIndex();
    if (key.empty() || at < 0) {
        toast("CHANNELS", "Pick a channel in a list first");
        return;
    }
    evo_chanprefs_set_hidden(m_providerId.c_str(), key.c_str(), 1);
    evo_feedback(EVO_FB_CONFIRM);
    toast("CHANNELS", "Hidden - Options > Show hidden channels brings it back");
    const int left = (int)host.AllRows().size() - 1;
    host.ReloadFocusAt((size_t)(at < left ? at : (left > 0 ? left - 1 : 0)));
}

/* L2 / R2 on a channel, film or series: previous / next sort order. */
void ProviderHostScreen::stalkerSortAction(int dir)
{
    const char* id = evo_rmlui_provider_get_focused_id();
    char msg[120] = {0};
    int r = stalker_cycle_sort(id ? id : "", dir, msg, sizeof msg);
    if (msg[0]) toast("STALKER / MINISTRA", msg);
    if (r == 0) {
        evo_feedback(EVO_FB_CONFIRM);
        evo_rmlui_provider_reload();
    }
}

/* Triangle / L2 / R2 on a folder: hide it, move it, or remove a portal. */
void ProviderHostScreen::stalkerItemAction(int action)
{
    const char* id = evo_rmlui_provider_get_focused_id();
    char msg[120] = {0};
    int r = stalker_item_action(id ? id : "", action, msg, sizeof msg);
    if (msg[0]) toast("STALKER / MINISTRA", msg);
    if (r == 0) {
        evo_feedback(EVO_FB_CONFIRM);
        evo_rmlui_provider_reload();
    }
}

/* Called every frame from update(). */
void ProviderHostScreen::stalkerStep()
{
    switch (m_stalkerStep) {
    case StalkerStep::OpenMac: {
        std::string initial = "00:1A:79:";
        m_stalkerStep = StalkerStep::Mac;
        evo_keyboard_open_phone("MAC address (activated by your provider)", initial.c_str(), 24,
                                &ProviderHostScreen::OnStalkerMac, this);
        break;
    }
    case StalkerStep::Mac:
        if (!evo_keyboard_is_open()) m_stalkerStep = StalkerStep::None;   /* cancelled */
        break;
    default:
        break;
    }
}

} // namespace evo
