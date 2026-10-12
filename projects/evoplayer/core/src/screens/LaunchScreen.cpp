#include "evo/screens/LaunchScreen.hpp"
#include "evo_features.h"
#include "evo/screens/ProviderHostScreen.hpp"
#include "evo/Application.hpp"
#include "evo_rmlui_bridge.h"
#include "evo_recent.h"
#include "evo_favorites.h"
#include "evo_feedback.h"
#include "evo_toast.h"
#include "evo_nav.h"
#include "evo_provider.h"
#include "evo_provider_bundle.h"
#include "evo_title_tag.h"
#include "evo_tmdb.h"
#include "provider_stalker.h"

#include "evo_boot_trace.h"
#include "evo_boot_log.h"
#include "evo/animation/AnimationManager.hpp"

#include <cctype>
#include <cstdio>
#include <cstring>
#include <ctime>

#include <algorithm>
#include <cstdlib>
#include <string>
#include <vector>

namespace evo {

namespace {

enum LibAction { LIB_LIVETV, LIB_SETTINGS, LIB_ABOUT };

struct LibTile {
    const char* title;
    const char* detail;
    const char* icon;
    LibAction   action;
};

/*
 * The library shelf. IPTV only: Live TV (M3U, Xtream Codes, Stalker), then
 * Settings and About. The USB browser, Recent and Favorites lists are gone;
 * Continue Watching and Favorites live on Home itself.
 */
const LibTile* libraryTiles() {
    static const LibTile tiles[] = {
        {"LIVE TV",  "M3U playlists, Xtream Codes and Stalker portals", "../icons/icon_tv.png", LIB_LIVETV},
        {"SETTINGS", "Playback and display preferences",  "../icons/icon_settings.png",      LIB_SETTINGS},
        {"ABOUT",    "Credits and project info",          "../icons/icon_about_support.png", LIB_ABOUT},
    };
    return tiles;
}

int libraryTileCount() {
    return 3;
}

} // namespace


LaunchScreen::LaunchScreen()
    : StatefulScreen("LaunchScreen")
    , m_launchFsm(LaunchScreenState::HeroFocused, "LaunchFSM")
{
    initLaunchStateMachine();
}

void LaunchScreen::initLaunchStateMachine() {
    m_launchFsm
        .addState(LaunchScreenState::HeroFocused, "HeroFocused")
        .addState(LaunchScreenState::RecentShelf, "RecentShelf")
        .addState(LaunchScreenState::LibraryShelf, "LibraryShelf");

    m_launchFsm
        .addTransition(LaunchScreenState::HeroFocused, LaunchScreenEvent::FocusRecent, LaunchScreenState::RecentShelf)
        .addTransition(LaunchScreenState::HeroFocused, LaunchScreenEvent::FocusLibrary, LaunchScreenState::LibraryShelf)
        .addTransition(LaunchScreenState::RecentShelf, LaunchScreenEvent::FocusHero, LaunchScreenState::HeroFocused)
        .addTransition(LaunchScreenState::RecentShelf, LaunchScreenEvent::FocusLibrary, LaunchScreenState::LibraryShelf)
        .addTransition(LaunchScreenState::LibraryShelf, LaunchScreenEvent::FocusRecent, LaunchScreenState::RecentShelf)
        .addTransition(LaunchScreenState::LibraryShelf, LaunchScreenEvent::FocusHero, LaunchScreenState::HeroFocused);
}

void LaunchScreen::onEnter() {
    StatefulScreen::onEnter();
    evo::animation::AnimationManager::getInstance().setContinuousAnimation(true);
    m_optionsOpen = false;
    m_confirmClearOpen = false;
    m_detailsOpen = false;
    /* Watched to the end more than a week ago: it has done its job on Home. */
    if (const int gone = recent_prune_watched(7))
        evo_boot_log("home: %d watched item(s) older than a week left Continue Watching", gone);
    m_selectedRow = RowHero;
    m_selectedCol = 0;
    keepSelectionValid();
    m_heroSettlePath.clear();
    m_heroSettleMs = 0.0;
    if (m_selectedRow == RowHero) m_launchFsm.postEvent(LaunchScreenEvent::FocusHero);
    else if (m_selectedRow == RowLibrary) m_launchFsm.postEvent(LaunchScreenEvent::FocusLibrary);
    else m_launchFsm.postEvent(LaunchScreenEvent::FocusRecent);
    /* Home shows up to ~40 posters at once (rows of films and series): room
     * for them, so none is thrown away while it is on screen. */
    static bool s_budgetSet = false;
    if (!s_budgetSet) {
        s_budgetSet = true;
        evo_provider_art_set_budget(40u << 20);
    }
}

void LaunchScreen::onExit() {
    evo::animation::AnimationManager::getInstance().setContinuousAnimation(false);
    StatefulScreen::onExit();
}

namespace {

bool recentFinished(const RecentFileEntry& r) {
    return r.duration > 1.0 && (r.last_pos / r.duration) >= 0.95;
}


/*
 * Episodes of one series are ONE tile on Home, showing the episode watched last.
 *
 * A provider episode is kept under "evo://<provider>/<portal>|se:<id>:<season>:<ep>",
 * so the series is everything before the last two colons; its title reads
 * "<Series> - S1E3". Either one is enough to group by (older entries only have
 * the path, other providers only the title).
 */
struct EpisodeInfo {
    bool        is = false;
    std::string key;          /* what the episodes of one series share */
    std::string series;       /* "" when only the path is known */
    int         season = 0;   /* 0 = the series has no seasons */
    int         ep = 0;
};

bool parseEpisodeTitle(const char* title, std::string& series, int& season, int& ep) {
    const std::string t(title ? title : "");
    const size_t pos = t.rfind(" - ");
    if (pos == std::string::npos || pos == 0) return false;
    const char* c = t.c_str() + pos + 3;
    int sn = 0, en = 0;
    if (*c == 'S') {
        ++c;
        if (!std::isdigit(static_cast<unsigned char>(*c))) return false;
        while (std::isdigit(static_cast<unsigned char>(*c))) sn = sn * 10 + (*c++ - '0');
    }
    if (*c != 'E') return false;
    ++c;
    if (!std::isdigit(static_cast<unsigned char>(*c))) return false;
    while (std::isdigit(static_cast<unsigned char>(*c))) en = en * 10 + (*c++ - '0');
    if (*c) return false;
    series = t.substr(0, pos);
    season = sn;
    ep = en;
    return true;
}

bool parsePathEpisode(const char* path, std::string& key, int& season, int& ep) {
    if (std::strncmp(path, "evo://", 6) != 0) return false;
    const char* bar = std::strchr(path + 6, '|');
    if (!bar || (bar[1] != 's' && bar[1] != 'v') || bar[2] != 'e' || bar[3] != ':') return false;
    const std::string p(path);
    const size_t afterKind = static_cast<size_t>(bar - path) + 3;
    const size_t c1 = p.rfind(':');
    if (c1 == std::string::npos || c1 <= afterKind) return false;
    const size_t c2 = p.rfind(':', c1 - 1);
    if (c2 == std::string::npos || c2 <= afterKind) return false;
    ep = std::atoi(p.c_str() + c1 + 1);
    const int si = std::atoi(p.c_str() + c2 + 1);
    season = (si >= 0) ? si + 1 : 0;
    key = p.substr(0, c2);
    return true;
}

EpisodeInfo episodeInfo(const RecentFileEntry& r) {
    EpisodeInfo e;
    if (std::strncmp(r.path, "evo://", 6) != 0) return e;   /* a local file is never grouped */
    std::string pkey, ser;
    int ps = 0, pe = 0, ts = 0, te = 0;
    const bool byPath = parsePathEpisode(r.path, pkey, ps, pe);
    const bool byTitle = parseEpisodeTitle(r.title, ser, ts, te);
    if (!byPath && !byTitle) return e;
    e.is = true;
    e.season = byPath ? ps : ts;
    e.ep = byPath ? pe : te;
    if (byTitle) e.series = ser;
    if (byPath) {
        e.key = pkey;
    } else {
        const char* rest = r.path + 6;
        const char* slash = std::strchr(rest, '/');
        e.key = "T:" + std::string(rest, slash ? static_cast<size_t>(slash - rest) : std::strlen(rest)) +
                ":" + ser;
    }
    return e;
}

/* The shelf's order: one entry per series (its latest episode), unfinished first. */
int recentOrder(int* order) {
    const int count = std::min(recent_file_count, MAX_RECENT_FILES);
    int reps[MAX_RECENT_FILES];
    int nr = 0;
    std::vector<std::string> seen;
    for (int i = 0; i < count; ++i) {
        const EpisodeInfo e = episodeInfo(recent_files[i]);
        if (e.is) {
            if (std::find(seen.begin(), seen.end(), e.key) != seen.end()) continue;
            seen.push_back(e.key);
        }
        reps[nr++] = i;
    }
    int n = 0;
    for (int k = 0; k < nr; ++k) if (!recentFinished(recent_files[reps[k]])) order[n++] = reps[k];
    for (int k = 0; k < nr; ++k) if (recentFinished(recent_files[reps[k]])) order[n++] = reps[k];
    return n;
}

int recentCount() {
    int order[MAX_RECENT_FILES];
    return recentOrder(order);
}

/* "S1E4", or "E4" for a series without seasons. */
void episodeCode(const EpisodeInfo& e, char* out, size_t cap) {
    if (e.season > 0) std::snprintf(out, cap, "S%dE%d", e.season, e.ep);
    else              std::snprintf(out, cap, "E%d", e.ep);
}

/* What a tile calls the entry: "Scrubs (2026) S1E4" for an episode, the
 * title for anything else. An old entry that only kept "Episode 3" shows
 * just its code rather than "Episode 3 S1E3". */
const char* recentDisplayTitle(const RecentFileEntry& r, char* buf, size_t cap) {
    const EpisodeInfo e = episodeInfo(r);
    if (!e.is) {
        if (!r.title[0]) return r.path;
        evo_title_strip_tags(r.title, buf, cap);              /* "MULTI Dune" -> "Dune" */
        return buf;
    }
    char code[24];
    episodeCode(e, code, sizeof code);
    std::string name = e.series;
    if (name.empty() && r.title[0] && std::strncmp(r.title, "Episode ", 8) != 0)
        name = r.title;
    char clean[160];
    evo_title_strip_tags(name.c_str(), clean, sizeof clean);
    if (!clean[0]) std::snprintf(buf, cap, "%s", code);
    else           std::snprintf(buf, cap, "%s %s", clean, code);
    return buf;
}

/* The poster of a provider item: its decoded pixels once they have arrived. */
const uint32_t* providerPoster(const RecentFileEntry& r, int* w, int* h) {
    if (!r.art[0] || std::strncmp(r.path, "evo://", 6) != 0) return nullptr;
    const char* rest = r.path + 6;
    const char* slash = std::strchr(rest, '/');
    if (!slash) return nullptr;
    const std::string provider(rest, static_cast<size_t>(slash - rest));
    char key[128];
    if (evo_provider_art_request(provider.c_str(), r.art, key, sizeof key) != 1) return nullptr;
    return evo_provider_art_pixels(key, w, h);
}

/* "EMBY  -  34 MIN LEFT" / "WATCHED" / "LOCAL FILE  -  AT 12:34". The stored path of a
 * provider item is an internal key (evo://emby/1234), never something to show. */
void describeRecent(const RecentFileEntry& r, char* out, size_t cap) {
    char source[48];
    if (std::strncmp(r.path, "evo://", 6) == 0) {
        const char* rest = r.path + 6;
        const char* slash = std::strchr(rest, '/');
        std::string id(rest, slash ? static_cast<size_t>(slash - rest) : std::strlen(rest));
        const evo_provider_t* p = evo_provider_find(id.c_str());
        std::snprintf(source, sizeof source, "%s", p && p->name ? p->name : id.c_str());
        for (char* c = source; *c; ++c) *c = static_cast<char>(std::toupper(static_cast<unsigned char>(*c)));
    } else {
        std::snprintf(source, sizeof source, "LOCAL FILE");
    }

    char when[48] = "";
    if (recentFinished(r)) {
        std::snprintf(when, sizeof when, "WATCHED");
    } else if (r.duration > 1.0 && r.last_pos > 1.0) {
        const long left = static_cast<long>(r.duration - r.last_pos);
        const long mins = (left + 30) / 60;
        if (mins >= 60) std::snprintf(when, sizeof when, "%ld H %02ld MIN LEFT", mins / 60, mins % 60);
        else            std::snprintf(when, sizeof when, "%ld MIN LEFT", mins < 1 ? 1L : mins);
    } else if (r.last_pos > 1.0) {
        const long s = static_cast<long>(r.last_pos);
        if (s >= 3600) std::snprintf(when, sizeof when, "AT %ld:%02ld:%02ld", s / 3600, (s / 60) % 60, s % 60);
        else           std::snprintf(when, sizeof when, "AT %ld:%02ld", s / 60, s % 60);
    } else if (episodeInfo(r).is) {
        std::snprintf(when, sizeof when, "UP NEXT");
    } else {
        std::snprintf(when, sizeof when, "NOT STARTED");
    }
    /* "STALKER  -  2019  -  EPISODE 5 OF 10  -  34 MIN LEFT" */
    std::string line = source;
    if (r.year[0]) line += std::string("  -  ") + r.year;
    if (r.rating[0]) line += std::string("  -  ") + r.rating + " / 10";
    const EpisodeInfo ep = episodeInfo(r);
    if (ep.is && r.ep_total > 0 && ep.ep > 0 && ep.ep <= r.ep_total) {
        char epl[40];
        std::snprintf(epl, sizeof epl, "EPISODE %d OF %d", ep.ep, r.ep_total);
        line += std::string("  -  ") + epl;
    }
    line += std::string("  -  ") + when;
    std::snprintf(out, cap, "%s", line.c_str());
}

/* ------------------------------------------------------------------------- */
/* The rows under Continue Watching                                          */
/* ------------------------------------------------------------------------- */

struct ShelfItem {
    std::string key;          /* "evo://<provider>/<id>", or a file path */
    std::string title;
    std::string detail;       /* the year, or where it comes from */
    std::string art;          /* poster URL */
    bool        folder = false;
    int         progress = -1;   /* 0..1000, -1 = none */
    std::vector<std::pair<std::string, std::string>> versions;   /* key, portal title: 2+ = choose */
    std::string rating;       /* TMDB's score, "7.8" */
    std::string year;
    std::string overview;
};

/* The newest films [0] and series [1]: filled from the provider's reply. */
std::vector<ShelfItem> g_new[2];
unsigned               g_newGen[2] = {0, 0};
std::string            g_newProvider;

/* The portal the rows come from: Stalker first, Xtream otherwise. */
const evo_provider_t* newRowsProvider() {
    static const char* const order[] = {"stalker", "xtream"};
    for (const char* id : order) {
        const evo_provider_t* p = evo_provider_find(id);
        if (p && p->list_catalog && p->is_configured && p->is_configured() && evo_provider_is_enabled(id))
            return p;
    }
    return nullptr;
}

std::string providerOf(const std::string& key) {
    if (key.compare(0, 6, "evo://") != 0) return std::string();
    const size_t slash = key.find('/', 6);
    return slash == std::string::npos ? std::string() : key.substr(6, slash - 6);
}

/* Where a remembered key comes from: "STALKER", "XTREAM", "LOCAL FILE". */
std::string sourceName(const std::string& key) {
    const std::string id = providerOf(key);
    if (id.empty()) return "LOCAL FILE";
    const evo_provider_t* p = evo_provider_find(id.c_str());
    std::string name = (p && p->name) ? p->name : id;
    for (char& c : name) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return name;
}

/* A folder (a series, a category) rather than something to play. Ids are the
 * provider's own; these are the shapes Stalker and Xtream use. */
bool keyIsFolder(const std::string& key) {
    const std::string prov = providerOf(key);
    if (prov.empty()) return false;
    std::string inner = key.substr(6 + prov.size() + 1);
    const size_t bar = inner.find('|');
    if (bar != std::string::npos) inner = inner.substr(bar + 1);
    if (inner == "live" || inner == "vod" || inner == "series") return true;
    if (prov == "stalker") {
        if (inner.size() > 3 && inner[2] == ':' && (inner[0] == 'v' || inner[0] == 's') &&
            std::strchr("sczn", inner[1]))
            return true;
        return inner.compare(0, 2, "g:") == 0 || inner.compare(0, 3, "ar:") == 0 ||
               inner.compare(0, 3, "ad:") == 0;
    }
    if (prov == "xtream")
        return inner.compare(0, 12, "series_show:") == 0 || inner.compare(0, 14, "series_season:") == 0 ||
               inner.find("_cat:") != std::string::npos;
    return false;
}

const RecentFileEntry* recentFor(const std::string& key) {
    for (int i = 0; i < recent_file_count; ++i)
        if (key == recent_files[i].path) return &recent_files[i];
    return nullptr;
}

int progressOf(const RecentFileEntry* r) {
    if (!r || r->duration <= 1.0) return -1;
    if (r->last_pos / r->duration >= 0.95) return 1000;
    return r->last_pos > 1.0 ? static_cast<int>(r->last_pos / r->duration * 1000.0) : -1;
}

std::string cleanTitle(const char* title) {
    char clean[192];
    evo_title_strip_tags(title ? title : "", clean, sizeof clean);
    return clean[0] ? std::string(clean) : std::string(title ? title : "");
}

std::vector<ShelfItem> favoriteItems() {
    std::vector<ShelfItem> out;
    for (int i = 0; i < favorite_count && static_cast<int>(out.size()) < EVO_RMLUI_SHELF_TILES; ++i) {
        const FavoriteEntry& f = favorite_files[i];
        ShelfItem s;
        s.key = f.path;
        s.title = cleanTitle(f.title);
        s.detail = sourceName(s.key);
        const RecentFileEntry* r = recentFor(s.key);
        s.art = f.art[0] ? f.art : (r ? r->art : "");
        s.folder = keyIsFolder(s.key);
        s.progress = progressOf(r);
        out.push_back(s);
    }
    return out;
}

struct NewCtx {
    int         kind;
    unsigned    gen;
    std::string provider;
};

void onNewItems(int ok, const evo_provider_item_t* items, int count, int /*more*/, void* ud) {
    NewCtx* c = static_cast<NewCtx*>(ud);
    const int kind = c->kind;
    const unsigned gen = c->gen;
    const std::string prov = c->provider;
    delete c;
    if (gen != g_newGen[kind]) return;                  /* a newer request is on its way */
    if (!ok || !items) {
        evo_boot_log("home: newest %s from %s: nothing", kind ? "series" : "films", prov.c_str());
        return;                                         /* keep what was shown */
    }
    std::vector<ShelfItem> out;
    for (int i = 0; i < count && static_cast<int>(out.size()) < EVO_RMLUI_SHELF_TILES; ++i) {
        const evo_provider_item_t& it = items[i];
        if (!it.id[0]) continue;
        ShelfItem s;
        s.key = "evo://" + prov + "/" + it.id;
        s.title = cleanTitle(it.title);
        s.detail = it.subtitle;
        /* "2022-06-23" -> "2022" */
        if (s.detail.size() >= 4 && std::isdigit(static_cast<unsigned char>(s.detail[0])) &&
            std::isdigit(static_cast<unsigned char>(s.detail[3])) && (s.detail.size() == 4 || s.detail[4] == '-'))
            s.detail = s.detail.substr(0, 4);
        s.art = it.art_url;
        s.folder = it.is_folder != 0;
        if (it.played) s.progress = 1000;
        else if (it.resume_pos_sec > 0 && it.duration_sec > 0)
            s.progress = static_cast<int>(it.resume_pos_sec * 1000 / it.duration_sec);
        out.push_back(s);
    }
    evo_boot_log("home: newest %s from %s: %d", kind ? "series" : "films", prov.c_str(),
                 static_cast<int>(out.size()));
    g_new[kind].swap(out);
}

/* The decoded poster of a provider URL, once it has arrived. `budget` stops
 * asking once the download queue is full this frame. */
const uint32_t* posterFor(const std::string& key, const std::string& url, int* w, int* h, bool* budget) {
    if (url.compare(0, 4, "http") != 0) return nullptr;
    const std::string prov = providerOf(key);
    if (prov.empty() || !*budget) return nullptr;
    char akey[128];
    const int rc = evo_provider_art_request(prov.c_str(), url.c_str(), akey, sizeof akey);
    if (rc == -2) *budget = false;
    if (rc != 1) return nullptr;
    return evo_provider_art_pixels(akey, w, h);
}

/* Where a version is: "Portal 2", "Xtream", "M3U" - shown next to its name. */
std::string versionSource(const std::string& key) {
    const std::string prov = providerOf(key);
    if (prov == "stalker") {
        if (stalker_portal_count() <= 1) return "Stalker";
        const size_t at = key.find('/', 6);
        const int k = at == std::string::npos ? 0 : std::atoi(key.c_str() + at + 1);
        return "Portal " + std::to_string(k + 1);
    }
    if (prov == "xtream") return "Xtream";
    if (prov == "iptv") return "M3U";
    if (prov == "emby") return "Emby";
    if (prov == "jellyfin") return "Jellyfin";
    return prov;
}

std::string versionLabel(const std::pair<std::string, std::string>& v) {
    return v.second + "   (" + versionSource(v.first) + ")";
}

/* TMDB's titles that the portal has (the others are not shown). */
std::vector<ShelfItem> tmdbItems(int list) {
    std::vector<ShelfItem> out;
    const evo_tmdb_item_t* items = nullptr;
    const int n = evo_tmdb_list(list, &items);
    for (int i = 0; i < n && static_cast<int>(out.size()) < EVO_RMLUI_SHELF_TILES; ++i) {
        const evo_tmdb_item_t& t = items[i];
        if (t.state != 1 || !t.key[0]) continue;
        bool twice = false;                         /* the portal has one item for two TMDB ids */
        for (const ShelfItem& o : out) twice = twice || o.key == t.key;
        if (twice) continue;
        ShelfItem s;
        s.key = t.key;
        s.title = t.local_title[0] ? t.local_title : t.title;   /* in the TMDB language */
        s.detail = t.year;
        s.art = t.poster;
        s.folder = t.folder != 0;
        s.progress = progressOf(recentFor(s.key));
        s.rating = t.rating;
        s.year = t.year;
        s.overview = t.overview;
        const int nv = evo_tmdb_version_count(&t);
        for (int v = 0; v < nv; ++v) {
            char vk[EVO_PROVIDER_MAX_ITEM_ID + 32], vt[128];
            if (evo_tmdb_version(&t, v, vk, sizeof vk, vt, sizeof vt))
                s.versions.emplace_back(vk, vt);
        }
        out.push_back(s);
    }
    return out;
}

/* The rows under Continue Watching, top to bottom (shelf index = row - 2). */
enum { ShelfFav = 0, ShelfTrending = 1, ShelfPopFilms = 2, ShelfPopSeries = 3, ShelfNewFilms = 4,
       ShelfNewSeries = 5 };
/*
 * The rows are built once per frame: rowCount(), the cursor checks and the
 * drawing all ask for them, and each build copied every title, poster and
 * version list - some thirty times a frame.
 */
constexpr int kShelfCacheN = 8;
std::vector<ShelfItem> g_shelfCache[kShelfCacheN];
bool g_shelfValid[kShelfCacheN];

void invalidateShelves() {
    for (bool& v : g_shelfValid) v = false;
}

std::vector<ShelfItem> buildShelf(int shelf) {
    switch (shelf) {
    case ShelfFav:       return favoriteItems();
    case ShelfTrending:  return tmdbItems(EVO_TMDB_TRENDING);
    case ShelfPopFilms:  return tmdbItems(EVO_TMDB_POPULAR_FILMS);
    case ShelfPopSeries: return tmdbItems(EVO_TMDB_POPULAR_SERIES);
    case ShelfNewFilms:  return tmdbItems(EVO_TMDB_NEW_FILMS);
    case ShelfNewSeries: return tmdbItems(EVO_TMDB_NEW_SERIES);
    default:             return std::vector<ShelfItem>();
    }
}

const std::vector<ShelfItem>& shelfItems(int shelf) {
    static const std::vector<ShelfItem> none;
    if (shelf < 0 || shelf >= kShelfCacheN) return none;
    if (!g_shelfValid[shelf]) {
        g_shelfCache[shelf] = buildShelf(shelf);
        g_shelfValid[shelf] = true;
    }
    return g_shelfCache[shelf];
}

bool shelfItemAt(int shelf, int col, ShelfItem* out) {
    const std::vector<ShelfItem>& v = shelfItems(shelf);
    if (col < 0 || col >= static_cast<int>(v.size())) return false;
    *out = v[static_cast<size_t>(col)];
    return true;
}

} // namespace

int LaunchScreen::recentIndex(int position) const {
    int order[MAX_RECENT_FILES];
    const int n = recentOrder(order);
    return (position >= 0 && position < n) ? order[position] : position;
}

void LaunchScreen::playKey(const std::string& key, const std::string& title, double pos) {
    if (m_resumeBusy) return;
    auto playback = Application::getInstance().getPlaybackController();
    auto screenMgr = Application::getInstance().getScreenManager();
    if (!playback || !screenMgr) return;

    if (key.compare(0, 14, "evo://stalker/") == 0) {
        /* The name it is known by: the portal is searched for it if its link fails. */
        std::string name = title;
        for (const auto& v : m_versions)
            if (v.first == key && !v.second.empty()) { name = v.second; break; }
        stalker_hint_title(key.c_str() + 14, name.c_str());
    }
    std::string error;
    switch (ProviderHostScreen::beginHeadlessResume(key.c_str(), title.c_str(), pos, error)) {
    case ProviderHostScreen::HeadlessBegin::Started:
        m_resumeBusy = true;
        m_resumeTitle = title.empty() ? key : title;
        return;
    case ProviderHostScreen::HeadlessBegin::NeedsPortal:
        ProviderHostScreen::resumeFromRecent(key.c_str(), title.c_str(), pos);   /* sign-in lives there */
        return;
    case ProviderHostScreen::HeadlessBegin::Failed:
        evo_feedback(EVO_FB_ERROR);
        toast("HOME", error.c_str());
        return;
    case ProviderHostScreen::HeadlessBegin::NotProvider:
        break;
    }
    playback->startPlayback(key.c_str(), pos);
    screenMgr->navigateTo(ScreenId::Player);
}

void LaunchScreen::resumeEntry(int idx) {
    if (idx < 0 || idx >= recent_file_count) return;
    m_retryVersions.clear();
    const RecentFileEntry& r = recent_files[idx];
    /* A finished film starts over; anything else carries on where it stopped. */
    const double pos = recentFinished(r) ? 0.0 : r.last_pos;
    playKey(r.path, r.title, pos);
}

/*
 * What the big tile describes: the tile under the cursor in Jump Back In,
 * otherwise the most recent one. It used to stay on the first, so moving along
 * the row showed the same title, story and progress whatever was focused.
 */
int LaunchScreen::heroIndex() const {
    if (m_selectedRow == RowRecent && m_selectedCol >= 0 && m_selectedCol < recentCount())
        return recentIndex(m_selectedCol);
    return recentIndex(0);
}

std::string LaunchScreen::heroPathForSelection() const {
    /*
     * The hero is the most recently played file and nothing else. It used to
     * follow the recent-shelf cursor and to be overwritten by whichever library
     * tile was hovered, so moving around Home kept rewriting the one panel that
     * is supposed to sit still and say "here is what you were watching".
     */
    if (recent_file_count <= 0) {
        return std::string();
    }
    return std::string(recent_files[heroIndex()].path);
}

/* ------------------------------------------------------------------------- */
/* Rows                                                                      */
/* ------------------------------------------------------------------------- */

int LaunchScreen::rowCount(int row) const {
    switch (row) {
    case RowHero:      return (recent_file_count > 0 || m_resumeBusy) ? 1 : 0;   /* nothing to continue: no button */
    case RowRecent:    return recent_file_count > 0 ? std::min(recentCount(), EVO_RMLUI_TILES) : 0;
    case RowLibrary:   return 0;   /* the side menu has the library; Home no longer repeats it */
    default:
        if (row >= RowFav && row < RowLibrary) return static_cast<int>(shelfItems(row - RowFav).size());
        return 0;
    }
}

/* A row can empty under the cursor (a favourite removed, the portal signed
 * out): move to the nearest row that still has something. */
void LaunchScreen::keepSelectionValid() {
    if (m_selectedRow < 0 || m_selectedRow >= RowCount) m_selectedRow = RowHero;
    if (rowCount(m_selectedRow) == 0) {
        int row = -1;
        for (int d = 1; d < RowCount && row < 0; ++d) {
            if (m_selectedRow - d >= 0 && rowCount(m_selectedRow - d) > 0) row = m_selectedRow - d;
            else if (m_selectedRow + d < RowCount && rowCount(m_selectedRow + d) > 0) row = m_selectedRow + d;
        }
        m_selectedRow = row < 0 ? RowHero : row;
        m_selectedCol = 0;
    }
    const int n = rowCount(m_selectedRow);
    if (m_selectedCol >= n) m_selectedCol = n > 0 ? n - 1 : 0;
    if (m_selectedCol < 0) m_selectedCol = 0;
}

void LaunchScreen::refreshNewRows(bool force) {
    const evo_provider_t* p = newRowsProvider();
    if (!p) {
        g_new[0].clear();
        g_new[1].clear();
        g_newProvider.clear();
        return;
    }
    if (g_newProvider != p->id) {                 /* another portal: its own rows */
        g_new[0].clear();
        g_new[1].clear();
        g_newProvider = p->id;
        force = true;
    }
    /* Every half hour; the portal side keeps its lists a day, so this is cheap.
     * Nothing yet: try again after 20 seconds (the network may not be up),
     * three times, then every ten minutes - an offline portal is not asked
     * over and over. */
    static int s_emptyTries = 0;
    const bool empty = g_new[0].empty() && g_new[1].empty();
    if (!empty) s_emptyTries = 0;
    const double wait = !empty ? 30.0 * 60.0 * 1000.0 : (s_emptyTries < 3 ? 20000.0 : 10.0 * 60.0 * 1000.0);
    if (!force && m_newRowsAgeMs < wait) return;
    if (empty) s_emptyTries++;
    m_newRowsAgeMs = 0.0;
    for (int kind = 0; kind < 2; ++kind) {
        NewCtx* ctx = new NewCtx{kind, ++g_newGen[kind], p->id};
        p->list_catalog(kind ? EVO_PROVIDER_NEW_SERIES : EVO_PROVIDER_NEW_FILMS, 0, onNewItems, ctx);
    }
}

void LaunchScreen::update(double deltaMs) {
    StatefulScreen::update(deltaMs);
    invalidateShelves();          /* rows are rebuilt once this frame */

    if (m_resumeBusy) {
        std::string error;
        switch (ProviderHostScreen::pollHeadlessResume(error)) {
        case ProviderHostScreen::HeadlessPoll::Ready:
            m_resumeBusy = false;
            m_retryVersions.clear();
            if (auto sm = Application::getInstance().getScreenManager())
                sm->navigateTo(ScreenId::Player);
            return;
        case ProviderHostScreen::HeadlessPoll::Failed:
            m_resumeBusy = false;
            if (!m_retryVersions.empty()) {
                /* This version would not play: the next one. */
                const auto next = m_retryVersions.front();
                m_retryVersions.erase(m_retryVersions.begin());
                toast("HOME", "That version would not play - trying another one");
                recent_hint_art(next.first.c_str(), m_retryArt.c_str());
                playKey(next.first, next.second, 0.0);
                break;
            }
            evo_feedback(EVO_FB_ERROR);
            if (!error.empty()) toast("CONTINUE WATCHING", error.c_str());
            break;
        case ProviderHostScreen::HeadlessPoll::Idle:
            m_resumeBusy = false;
            break;
        case ProviderHostScreen::HeadlessPoll::Working:
            break;
        }
    }
    m_launchFsm.update(deltaMs);
    evo_provider_art_poll();          /* posters arrive over the network */

    m_newRowsAgeMs += deltaMs;   /* the portal's own newest lists are no longer on Home */
    /* TRENDING / POPULAR: TMDB's lists, looked up in the same portal. */
    evo_tmdb_tick(nullptr);       /* every portal, account and playlist is searched */
    {
        char why[160];
        if (evo_tmdb_take_error(why, sizeof why)) toast("TMDB", why);
    }
    if (!m_optionsOpen && !m_confirmClearOpen) keepSelectionValid();

    /* Restart the debounce whenever the cursor lands on a different file. */
    std::string heroPath = heroPathForSelection();
    if (heroPath != m_heroSettlePath) {
        m_heroSettlePath = heroPath;
        m_heroSettleMs = 0.0;
    } else if (m_heroSettleMs < HeroSettleMs) {
        m_heroSettleMs += deltaMs;
    }
}

void LaunchScreen::navigate(int dx, int dy) {
    evo::animation::AnimationManager::getInstance().triggerTransition(350.0);
    if (dy != 0) {
        int row = m_selectedRow + dy;
        while (row >= 0 && row < RowCount && rowCount(row) == 0) row += dy;   /* skip empty rows */
        if (row < 0 || row >= RowCount) {
            evo_feedback(EVO_FB_BOUNDARY);
        } else {
            m_selectedRow = row;
            evo_feedback(EVO_FB_MOVE);
            if (row == RowHero) m_launchFsm.postEvent(LaunchScreenEvent::FocusHero);
            else if (row == RowLibrary) m_launchFsm.postEvent(LaunchScreenEvent::FocusLibrary);
            else m_launchFsm.postEvent(LaunchScreenEvent::FocusRecent);
        }
    }

    const int maxCol = std::max(0, rowCount(m_selectedRow) - 1);
    if (dx != 0) {
        int nextCol = m_selectedCol + dx;
        if (nextCol < 0) {
            m_selectedCol = 0;
            evo_feedback(EVO_FB_BOUNDARY);
        } else if (nextCol > maxCol) {
            m_selectedCol = maxCol;
            evo_feedback(EVO_FB_BOUNDARY);
        } else {
            m_selectedCol = nextCol;
            evo_feedback(EVO_FB_MOVE);
        }
    } else {
        if (m_selectedCol > maxCol) m_selectedCol = maxCol;
        if (m_selectedCol < 0) m_selectedCol = 0;
    }
}

/* Cross on a poster: a film plays (from where it stopped), a series or a
 * category opens in its portal, a local favourite plays. */
void LaunchScreen::activateShelf(int row, int col) {
    ShelfItem item;
    if (!shelfItemAt(row - RowFav, col, &item)) return;
    m_hintYear = item.year;
    m_hintRating = item.rating;
    m_hintOverview = item.overview;
    if (item.versions.size() > 1) {               /* the portal has it more than once: choose */
        openVersionMenu(item.title, item.art, item.folder, item.versions);
        return;
    }
    startKey(item.key, item.title, item.art, item.folder);
}

/* Start a film (where it stopped) or open a series, with Home's poster. */
void LaunchScreen::startKey(const std::string& key, const std::string& title, const std::string& art,
                            bool folder) {
    recent_hint_art(key.c_str(), art.c_str());    /* Continue Watching keeps this poster */
    recent_hint_info(key.c_str(), m_hintYear.c_str(), m_hintRating.c_str(), m_hintOverview.c_str());
    if (folder) {
        ProviderHostScreen::openFromHome(key.c_str(), title.c_str(), false);
        return;
    }
    /* If this version will not play, the others are tried after it. */
    m_retryVersions.clear();
    m_retryArt = art;
    for (const auto& v : m_versions)
        if (v.first != key) m_retryVersions.push_back(v);
    const RecentFileEntry* r = recentFor(key);
    const bool done = r && progressOf(r) >= 1000;
    playKey(key, r && r->title[0] ? r->title : title, (r && !done) ? r->last_pos : 0.0);
}

void LaunchScreen::openVersionMenu(const std::string& title, const std::string& art, bool folder,
                                   const std::vector<std::pair<std::string, std::string>>& versions) {
    m_versions = versions;
    m_menu.clear();
    m_optionsFocus = 0;
    m_menuTitle = title + "  -  choose a version";
    m_menuItemTitle = title;
    m_menuArt = art;
    m_menuFolder = folder;
    for (size_t i = 0; i < versions.size(); ++i) {
        const RecentFileEntry* r = recentFor(versions[i].first);
        const int pr = progressOf(r);
        const char* detail = folder ? "Open this version in the portal"
                           : pr >= 1000 ? "Watched"
                           : pr >= 0 ? "Started - carries on where you stopped" : "Play this version";
        m_menu.push_back({1000 + static_cast<int>(i), versionLabel(versions[i]), detail});
    }
    m_menuRow = -1;                               /* not about a Home tile */
    m_optionsOpen = true;
    evo_feedback(EVO_FB_OPEN);
}

void LaunchScreen::activateSelection() {
    auto screenMgr = Application::getInstance().getScreenManager();
    if (!screenMgr) return;

    if (m_selectedRow == RowHero) {
        if (recent_file_count > 0) {
            resumeEntry(recentIndex(0));
        }
    } else if (m_selectedRow == RowRecent) {
        if (m_selectedCol >= 0 && m_selectedCol < recentCount()) {
            resumeEntry(recentIndex(m_selectedCol));
        }
    } else if (m_selectedRow >= RowFav && m_selectedRow < RowLibrary) {
        openDetails(m_selectedRow, m_selectedCol);    /* its page first: what it is, then play */
    } else if (m_selectedRow == RowLibrary) {
        // Library shelf navigation. Dispatch on the tile's action rather than
        // its column, so hiding a tile cannot silently shift what the ones
        // after it do.
        if (m_selectedCol < 0 || m_selectedCol >= libraryTileCount())
            return;
        switch (libraryTiles()[m_selectedCol].action) {
            /* #90: the provider host - M3U, Xtream Codes and Stalker. */
            case LIB_LIVETV:   screenMgr->navigateTo(ScreenId::EmbyBrowse); break;
            case LIB_SETTINGS: screenMgr->navigateTo(ScreenId::Settings); break;
            case LIB_ABOUT:    screenMgr->navigateTo(ScreenId::AboutSupport); break;
            default: break;
        }
    }
}

void LaunchScreen::removeRecentAt(int index) {
    bool removed = false;
    if (index >= 0 && index < recent_file_count) {
        const EpisodeInfo ep = episodeInfo(recent_files[index]);
        if (ep.is) {
            /* The tile stands for the whole series: take every episode with it,
             * or the previous one would simply take its place. */
            const std::string key = ep.key;
            for (int i = recent_file_count - 1; i >= 0; --i)
                if (episodeInfo(recent_files[i]).key == key && recent_remove_at(i)) removed = true;
        } else {
            removed = recent_remove_at(index) != 0;
        }
    }
    if (!removed) {
        evo_feedback(EVO_FB_BOUNDARY);
        return;
    }

    evo_feedback(EVO_FB_CANCEL);
    toast("CONTINUE WATCHING", "Removed");
    evo::animation::AnimationManager::getInstance().triggerTransition(350.0);
    // Keep the cursor on something that still exists (the hero simply shows
    // the next most recent file).
    if (recent_file_count <= 0 && m_selectedRow <= RowRecent) {
        m_selectedRow = RowLibrary;
        m_selectedCol = 0;
        m_launchFsm.postEvent(LaunchScreenEvent::FocusLibrary);
    }
    keepSelectionValid();
}

/* Square: drop the focused Continue Watching entry, or the focused favourite. */
void LaunchScreen::removeFocusedRecent() {
    if (m_selectedRow == RowFav) {
        ShelfItem item;
        if (!shelfItemAt(ShelfFav, m_selectedCol, &item)) { evo_feedback(EVO_FB_BOUNDARY); return; }
        const ShelfItem* it = &item;
        favorites_remove(it->key.c_str());
        favorites_save();
        evo_feedback(EVO_FB_CANCEL);
        toast("FAVORITES", "Removed from favorites");
        keepSelectionValid();
        return;
    }
    int index = -1;
    if (m_selectedRow == RowHero) {
        index = recentIndex(0);                // the hero
    } else if (m_selectedRow == RowRecent) {
        index = recentIndex(m_selectedCol);    // recent shelf, as arranged on Home
    }
    if (index < 0 || index >= recent_file_count) {
        evo_feedback(EVO_FB_BOUNDARY);
        return;
    }
    removeRecentAt(index);
}

/* "Go to series": open the series in its portal to pick another episode. */
void LaunchScreen::goToSeries(int idx) {
    if (idx < 0 || idx >= recent_file_count) return;
    const RecentFileEntry& r = recent_files[idx];
    const EpisodeInfo e = episodeInfo(r);
    const std::string key = r.path;
    /* Stalker: "evo://stalker/0|se:123:0:4" lives in the folder "0|ss:123". */
    if (providerOf(key) == "stalker") {
        const size_t bar = key.find('|');
        const size_t at = (bar == std::string::npos) ? key.find('/', 6) + 1 : bar + 1;
        if (key.size() > at + 3 && key[at + 1] == 'e' && key[at + 2] == ':') {
            const size_t end = key.find(':', at + 3);
            const std::string id = key.substr(at + 3, end == std::string::npos ? std::string::npos : end - at - 3);
            const std::string folder = key.substr(0, at) + key[at] + "s:" + id;
            const std::string name = e.series.empty() ? std::string("Series") : cleanTitle(e.series.c_str());
            ProviderHostScreen::openFromHome(folder.c_str(), name.c_str(), false);
            return;
        }
    }
    /* Anything else: search the portal for the series' name. */
    if (!e.series.empty()) {
        ProviderHostScreen::openFromHome(key.c_str(), cleanTitle(e.series.c_str()).c_str(), true);
        return;
    }
    evo_feedback(EVO_FB_ERROR);
    toast("HOME", "The series of this episode is not known");
}

/* Mark a film from a row watched, or not watched again. */
void LaunchScreen::toggleShelfWatched(const std::string& key, const std::string& title, const std::string& art) {
    int at = -1;
    for (int i = 0; i < recent_file_count; ++i)
        if (key == recent_files[i].path) { at = i; break; }
    if (at >= 0 && progressOf(&recent_files[at]) >= 1000) {
        recent_remove_at(at);
        toast("WATCHED", "Marked as not watched");
        evo_feedback(EVO_FB_CANCEL);
        return;
    }
    if (at >= 0) {
        recent_mark_watched(at);
    } else {
        recent_add_or_update(key.c_str(), title.c_str(), 100.0, 100.0);
        if (!art.empty()) recent_set_art(key.c_str(), art.c_str());
        recent_save();
    }
    toast("WATCHED", "Marked as watched");
    evo_feedback(EVO_FB_CONFIRM);
}

/* ------------------------------------------------------------------------- */
/* Options menu                                                              */
/* ------------------------------------------------------------------------- */

namespace {
enum MenuCode {
    MenuPlay, MenuStartOver, MenuGoToSeries, MenuMarkWatched, MenuMarkUnwatched, MenuResetProgress,
    MenuRemove, MenuClearAll, MenuOpen, MenuFavAdd, MenuFavRemove, MenuShelfWatched, MenuChooseVersion
};
} // namespace

void LaunchScreen::openOptionsMenu() {
    m_menu.clear();
    m_optionsFocus = 0;
    m_menuRow = m_selectedRow;
    m_menuCol = m_selectedCol;
    auto add = [this](int code, const char* label, const char* detail) {
        m_menu.push_back({code, label, detail});
    };

    if (m_selectedRow == RowHero || m_selectedRow == RowRecent) {
        const int index = (m_selectedRow == RowHero) ? recentIndex(0) : recentIndex(m_selectedCol);
        if (recent_file_count <= 0 || index < 0 || index >= recent_file_count) {
            evo_feedback(EVO_FB_BOUNDARY);
            return;
        }
        m_optionsTarget = index;
        const RecentFileEntry& r = recent_files[index];
        char title[160];
        m_menuTitle = recentDisplayTitle(r, title, sizeof title);
        const bool done = recentFinished(r);
        const bool started = !done && r.last_pos > 1.0;
        const bool episode = episodeInfo(r).is;
        add(MenuPlay, done ? "Play again" : started ? "Resume" : "Play",
            started ? "Carry on where you stopped" : "Start it now");
        if (started) add(MenuStartOver, "Start over", "Play it from the beginning");
        if (episode && providerOf(r.path).size())
            add(MenuGoToSeries, "Go to series", "Open the series in the portal and pick an episode");
        if (!done) add(MenuMarkWatched, "Mark as watched", "As if you had watched it to the end");
        else       add(MenuMarkUnwatched, "Mark as not watched", "Its progress goes back to the start");
        if (started) add(MenuResetProgress, "Reset progress", "Keep it here, start from the top next time");
        add(MenuRemove, episode ? "Remove the series from Home" : "Remove from Home",
            "Out of Continue Watching - nothing is deleted");
        add(MenuClearAll, "Clear Continue Watching", "Empty the whole row");
    } else if (m_selectedRow >= RowFav && m_selectedRow < RowLibrary) {
        ShelfItem item;
        if (!shelfItemAt(m_selectedRow - RowFav, m_selectedCol, &item)) { evo_feedback(EVO_FB_BOUNDARY); return; }
        const ShelfItem* it = &item;
        m_menuKey = it->key;
        m_hintYear = it->year;
        m_hintRating = it->rating;
        m_hintOverview = it->overview;
        m_menuItemTitle = it->title;
        m_menuArt = it->art;
        m_menuFolder = it->folder;
        m_menuTitle = it->title;
        const RecentFileEntry* r = recentFor(it->key);
        if (it->folder) {
            add(MenuOpen, "Open", "Show it in the portal");
        } else {
            const bool started = r && progressOf(r) >= 0 && progressOf(r) < 1000;
            add(MenuPlay, started ? "Resume" : "Play", started ? "Carry on where you stopped" : "Start it now");
            if (started) add(MenuStartOver, "Start over", "Play it from the beginning");
        }
        if (it->versions.size() > 1) {
            m_versions = it->versions;
            static char vers[64];
            std::snprintf(vers, sizeof vers, "The portal has %d versions - pick one",
                          static_cast<int>(it->versions.size()));
            add(MenuChooseVersion, "Choose a version", vers);
        }
        if (favorites_find(it->key.c_str()) >= 0)
            add(MenuFavRemove, "Remove from favorites", "Off the Favorites row");
        else
            add(MenuFavAdd, "Add to favorites", "On the Favorites row on Home");
        if (!it->folder && !providerOf(it->key).empty())
            add(MenuShelfWatched, (r && progressOf(r) >= 1000) ? "Mark as not watched" : "Mark as watched",
                "The check mark in the portal follows it");
    } else {
        evo_feedback(EVO_FB_BOUNDARY);   /* the library has nothing to set */
        return;
    }
    m_optionsOpen = true;
    evo_feedback(EVO_FB_CONFIRM);
}

void LaunchScreen::runOptionsAction(int action) {
    m_optionsOpen = false;
    if (action < 0 || action >= static_cast<int>(m_menu.size())) return;
    const int code = m_menu[static_cast<size_t>(action)].code;
    if (code >= 1000) {                           /* a version from the version menu */
        const size_t v = static_cast<size_t>(code - 1000);
        if (v < m_versions.size())
            startKey(m_versions[v].first, m_menuItemTitle, m_menuArt, m_menuFolder);
        return;
    }
    if (code == MenuChooseVersion) {
        const auto versions = m_versions;
        openVersionMenu(m_menuItemTitle, m_menuArt, m_menuFolder, versions);
        return;
    }
    const bool cw = (m_menuRow == RowHero || m_menuRow == RowRecent);
    if (cw && (m_optionsTarget < 0 || m_optionsTarget >= recent_file_count)) return;

    switch (code) {
    case MenuPlay:
        if (cw) resumeEntry(m_optionsTarget);
        else    activateShelf(m_menuRow, m_menuCol);
        break;
    case MenuOpen:
        activateShelf(m_menuRow, m_menuCol);
        break;
    case MenuStartOver:
        if (cw) playKey(recent_files[m_optionsTarget].path, recent_files[m_optionsTarget].title, 0.0);
        else    playKey(m_menuKey, m_menuItemTitle, 0.0);
        break;
    case MenuGoToSeries:
        goToSeries(m_optionsTarget);
        break;
    case MenuMarkWatched:
        if (recent_mark_watched(m_optionsTarget)) {
            toast("CONTINUE WATCHING", "Marked as watched");
            evo::animation::AnimationManager::getInstance().triggerTransition(350.0);
        }
        break;
    case MenuMarkUnwatched:
    case MenuResetProgress:
        if (recent_reset_progress(m_optionsTarget))
            toast("CONTINUE WATCHING", code == MenuMarkUnwatched ? "Marked as not watched" : "Progress reset");
        break;
    case MenuRemove:
        removeRecentAt(m_optionsTarget);
        break;
    case MenuClearAll:
        m_confirmClearFocus = 0;     // CANCEL is the default
        m_confirmClearOpen = true;
        break;
    case MenuFavAdd:
        favorites_add(m_menuKey.c_str(), m_menuItemTitle.c_str(), 0.0);
        favorites_set_art(m_menuKey.c_str(), m_menuArt.c_str());
        favorites_save();
        toast("FAVORITES", "Added to favorites");
        break;
    case MenuFavRemove:
        favorites_remove(m_menuKey.c_str());
        favorites_save();
        toast("FAVORITES", "Removed from favorites");
        keepSelectionValid();
        break;
    case MenuShelfWatched:
        toggleShelfWatched(m_menuKey, m_menuItemTitle, m_menuArt);
        break;
    default:
        break;
    }
}

bool LaunchScreen::handleOptionsInput(uint32_t pressed) {
    const int count = static_cast<int>(m_menu.size());
    if (pressed & PadButtons::Up) {
        if (m_optionsFocus > 0) { m_optionsFocus--; evo_feedback(EVO_FB_MOVE); }
        else evo_feedback(EVO_FB_BOUNDARY);
    } else if (pressed & PadButtons::Down) {
        if (m_optionsFocus < count - 1) { m_optionsFocus++; evo_feedback(EVO_FB_MOVE); }
        else evo_feedback(EVO_FB_BOUNDARY);
    } else if (pressed & PadButtons::Cross) {
        evo_feedback(EVO_FB_CONFIRM);
        runOptionsAction(m_optionsFocus);
    } else if (pressed & (PadButtons::Circle | PadButtons::Options | PadButtons::Left)) {
        evo_feedback(EVO_FB_CANCEL);
        m_optionsOpen = false;
    }
    return true;   // modal: nothing underneath sees the pad while it is open
}

void LaunchScreen::renderOptionsMenu(uint32_t* framebuffer, int width, int height) {
    evo_rmlui_list_params_t params;
    std::memset(&params, 0, sizeof(params));
    params.section = EVO_SECTION_HOME;
    params.rail_focused = 0;
    params.title = "MENU";
    params.subtitle = m_menuTitle.c_str();
    const int total = static_cast<int>(m_menu.size());
    params.total_count = total;
    params.cursor_index = total ? m_optionsFocus : -1;
    const int rows = std::min(EVO_RMLUI_LIST_ROWS, total);
    int first = 0;
    if (m_optionsFocus >= rows) first = m_optionsFocus - rows + 1;
    params.row_count = rows;
    for (int i = 0; i < rows; ++i) {
        const MenuItem& m = m_menu[static_cast<size_t>(first + i)];
        params.rows[i].title = m.label.c_str();
        params.rows[i].detail = m.detail.c_str();
        params.rows[i].icon_path =
            (m.code == MenuPlay || m.code == MenuOpen)          ? "../icons/btn_play.png"
          : (m.code == MenuStartOver || m.code == MenuResetProgress ||
             m.code == MenuMarkUnwatched)                         ? "../icons/icon_resume.png"
          : (m.code == MenuFavAdd || m.code == MenuFavRemove)    ? "../icons/icon_favorites.png"
          : (m.code == MenuRemove || m.code == MenuClearAll)     ? "../icons/icon_trash.png"
          : (m.code == MenuGoToSeries)                           ? "../icons/icon_folder.png"
          : (m.code >= 1000 || m.code == MenuChooseVersion)      ? "../icons/btn_play.png"
          :                                                         "../icons/icon_chevron.png";
        params.rows[i].badge = "";
        params.rows[i].progress = -1;
        params.rows[i].has_chevron = 1;
        params.rows[i].is_focused = (first + i == m_optionsFocus);
    }
    params.hint_count = 2;
    params.hints[0].glyph_path = "../icons/btn_cross.png";
    params.hints[0].label = "SELECT";
    params.hints[1].glyph_path = "../icons/btn_circle.png";
    params.hints[1].label = "BACK";
    evo_rmlui_update_list(&params);
    evo_rmlui_render_list(framebuffer, width, height);
}

/* ------------------------------------------------------------------------- */
/* Details page                                                              */
/* ------------------------------------------------------------------------- */

namespace {
enum DetailCode { DPlay = 1, DStartOver, DOpen, DFav, DWatched, DAllVersions, DVersion = 1000 };
} // namespace

void LaunchScreen::openDetails(int row, int col) {
    ShelfItem item;
    if (!shelfItemAt(row - RowFav, col, &item)) return;
    m_dKey = item.key;
    m_dTitle = item.title;
    m_dArt = item.art;
    m_dYear = item.year.empty() ? item.detail : item.year;
    m_dRating = item.rating;
    m_dOverview = item.overview;
    m_dFolder = item.folder;
    m_versions = item.versions;
    m_hintYear = item.year;
    m_hintRating = item.rating;
    m_hintOverview = item.overview;
    m_detailsFocus = 0;
    buildDetailButtons();
    m_detailsOpen = true;
    evo_feedback(EVO_FB_OPEN);
}

void LaunchScreen::buildDetailButtons() {
    m_dButtons.clear();
    auto add = [this](int code, const std::string& label, const std::string& detail) {
        m_dButtons.push_back({code, label, detail});
    };
    const bool provider = !providerOf(m_dKey).empty();
    if (m_versions.size() > 1) {
        /* One button per version: its own name says which ("|NL| Dune 4K"). */
        /* Room for six; with more, five and a list of them all. */
        const size_t shown = m_versions.size() > 6 ? 5 : m_versions.size();
        for (size_t i = 0; i < shown; ++i) {
            const RecentFileEntry* r = recentFor(m_versions[i].first);
            const int pr = progressOf(r);
            const std::string verb = m_dFolder ? "Open" : (pr >= 0 && pr < 1000) ? "Resume" : "Play";
            add(DVersion + static_cast<int>(i), verb + "  " + versionLabel(m_versions[i]),
                m_dFolder ? "In the portal" : pr >= 1000 ? "Watched" : "");
        }
        if (shown < m_versions.size())
            add(DAllVersions, "All " + std::to_string(m_versions.size()) + " versions", "Every portal, account and playlist that has it");
    } else if (m_dFolder) {
        add(DOpen, "Open series", "Seasons and episodes in the portal");
    } else {
        const RecentFileEntry* r = recentFor(m_dKey);
        const int pr = progressOf(r);
        if (pr >= 0 && pr < 1000 && r) {
            char at[48];
            const long sec = static_cast<long>(r->last_pos);
            if (sec >= 3600) std::snprintf(at, sizeof at, "from %ld:%02ld:%02ld", sec / 3600, (sec / 60) % 60, sec % 60);
            else             std::snprintf(at, sizeof at, "from %ld:%02ld", sec / 60, sec % 60);
            add(DPlay, "Resume", at);
            add(DStartOver, "Start over", "From the beginning");
        } else {
            add(DPlay, "Play", pr >= 1000 ? "You have seen it" : "");
        }
    }
    add(DFav, favorites_find(m_dKey.c_str()) >= 0 ? "Remove from favorites" : "Add to favorites", "");
    if (!m_dFolder && provider) {
        const int pr = progressOf(recentFor(m_dKey));
        add(DWatched, pr >= 1000 ? "Mark as not watched" : "Mark as watched", "");
    }
    if (m_detailsFocus >= static_cast<int>(m_dButtons.size())) m_detailsFocus = 0;
}

bool LaunchScreen::handleDetailsInput(uint32_t pressed) {
    const int n = static_cast<int>(m_dButtons.size());
    if (pressed & PadButtons::Up) {
        if (m_detailsFocus > 0) { m_detailsFocus--; evo_feedback(EVO_FB_MOVE); }
        else evo_feedback(EVO_FB_BOUNDARY);
    } else if (pressed & PadButtons::Down) {
        if (m_detailsFocus < n - 1) { m_detailsFocus++; evo_feedback(EVO_FB_MOVE); }
        else evo_feedback(EVO_FB_BOUNDARY);
    } else if (pressed & PadButtons::Cross) {
        if (m_detailsFocus >= 0 && m_detailsFocus < n) {
            evo_feedback(EVO_FB_CONFIRM);
            runDetailsAction(m_dButtons[static_cast<size_t>(m_detailsFocus)].code);
        }
    } else if (pressed & (PadButtons::Circle | PadButtons::Options)) {
        evo_feedback(EVO_FB_CANCEL);
        m_detailsOpen = false;
    }
    return true;   // modal
}

void LaunchScreen::runDetailsAction(int code) {
    if (code >= DVersion) {
        const size_t v = static_cast<size_t>(code - DVersion);
        if (v < m_versions.size()) {
            m_detailsOpen = false;
            startKey(m_versions[v].first, m_dTitle, m_dArt, m_dFolder);
        }
        return;
    }
    if (code == DAllVersions) {
        m_detailsOpen = false;
        openVersionMenu(m_dTitle, m_dArt, m_dFolder, m_versions);
        return;
    }
    switch (code) {
    case DPlay:
    case DOpen:
        m_detailsOpen = false;
        startKey(m_dKey, m_dTitle, m_dArt, m_dFolder);
        break;
    case DStartOver:
        m_detailsOpen = false;
        recent_hint_art(m_dKey.c_str(), m_dArt.c_str());
        recent_hint_info(m_dKey.c_str(), m_hintYear.c_str(), m_hintRating.c_str(), m_hintOverview.c_str());
        playKey(m_dKey, m_dTitle, 0.0);
        break;
    case DFav:
        if (favorites_find(m_dKey.c_str()) >= 0) {
            favorites_remove(m_dKey.c_str());
            toast("FAVORITES", "Removed from favorites");
        } else {
            favorites_add(m_dKey.c_str(), m_dTitle.c_str(), 0.0);
            favorites_set_art(m_dKey.c_str(), m_dArt.c_str());
            toast("FAVORITES", "Added to favorites");
        }
        favorites_save();
        buildDetailButtons();
        break;
    case DWatched:
        toggleShelfWatched(m_dKey, m_dTitle, m_dArt);
        buildDetailButtons();
        break;
    default:
        break;
    }
}

bool LaunchScreen::handleConfirmClearInput(uint32_t pressed) {
    if (pressed & (PadButtons::Left | PadButtons::Right | PadButtons::Up | PadButtons::Down)) {
        m_confirmClearFocus = (m_confirmClearFocus == 0) ? 1 : 0;
        evo_feedback(EVO_FB_MOVE);
    } else if (pressed & PadButtons::Cross) {
        m_confirmClearOpen = false;
        if (m_confirmClearFocus == 1) {
            recent_clear_all();
            evo_feedback(EVO_FB_CANCEL);
            toast("CONTINUE WATCHING", "Cleared");
            evo::animation::AnimationManager::getInstance().triggerTransition(350.0);
            m_selectedRow = RowLibrary;
            m_selectedCol = 0;
            keepSelectionValid();
            m_launchFsm.postEvent(LaunchScreenEvent::FocusLibrary);
        } else {
            evo_feedback(EVO_FB_CANCEL);
        }
    } else if (pressed & (PadButtons::Circle | PadButtons::Options)) {
        evo_feedback(EVO_FB_CANCEL);
        m_confirmClearOpen = false;
    }
    return true;   // modal
}

bool LaunchScreen::handleInput(uint32_t pressed, uint32_t held, uint32_t released) {
    (void)held;
    (void)released;

    if (m_resumeBusy) {
        return true;       // the stream is being found; a second press must not start another
    }
    if (m_confirmClearOpen) {
        return handleConfirmClearInput(pressed);
    }
    if (m_optionsOpen) {
        return handleOptionsInput(pressed);
    }
    if (m_detailsOpen) {
        return handleDetailsInput(pressed);
    }

    if (pressed & PadButtons::Options) {
        openOptionsMenu();
        return true;
    }

    if (pressed & PadButtons::Up) {
        navigate(0, -1);
        return true;
    }
    if (pressed & PadButtons::Down) {
        navigate(0, 1);
        return true;
    }
    if (pressed & PadButtons::Left) {
        if (m_selectedCol <= 0) {
            if (auto sm = Application::getInstance().getScreenManager()) {
                sm->setRailFocused(true);
                return true;
            }
        }
        navigate(-1, 0);
        return true;
    }
    if (pressed & PadButtons::Right) {
        navigate(1, 0);
        return true;
    }
    if (pressed & PadButtons::Cross) {
        evo_feedback(EVO_FB_CONFIRM);
        activateSelection();
        return true;
    }
    if (pressed & PadButtons::Square) {
        // Remove the focused Continue Watching item or favourite.
        removeFocusedRecent();
        return true;
    }
    if (pressed & PadButtons::Circle) {
        /* Back to the top: the big tile. */
        if (m_selectedRow != RowHero || m_selectedCol != 0) {
            evo_feedback(EVO_FB_CANCEL);
            evo::animation::AnimationManager::getInstance().triggerTransition(350.0);
            m_selectedRow = RowHero;
            m_selectedCol = 0;
            m_launchFsm.postEvent(LaunchScreenEvent::FocusHero);
        } else {
            evo_feedback(EVO_FB_BOUNDARY);
        }
        return true;
    }

    return false;
}

void LaunchScreen::render(uint32_t* framebuffer, int width, int height) {
    invalidateShelves();          /* a press this frame may have changed a row */
    if (m_optionsOpen) {                     /* the menu is a screen of its own */
        renderOptionsMenu(framebuffer, width, height);
        return;
    }

    evo_rmlui_launch_params_t params;
    std::memset(&params, 0, sizeof(params));

    params.app_name = "KIJKR";
    params.version = (EVO_PLAYER_VERSION[0] == 'v' || EVO_PLAYER_VERSION[0] == 'V')
                     ? EVO_PLAYER_VERSION
                     : ("v" EVO_PLAYER_VERSION);

    // Clock
    char clockBuf[16] = {0};
    time_t now = time(nullptr);
    struct tm tmv;
    if (localtime_r(&now, &tmv)) {
        std::snprintf(clockBuf, sizeof(clockBuf), "%02d:%02d", tmv.tm_hour, tmv.tm_min);
        params.clock = clockBuf;
    }

    auto coverService = Application::getInstance().getCoverArtService();

    bool railFocused = false;
    if (auto sm = Application::getInstance().getScreenManager()) {
        railFocused = sm->isRailFocused();
    }
    params.hero_focused = (!railFocused && m_selectedRow == RowHero);

    // Hero: the Jump Back In tile under the cursor, else the most recent file.
    if (recent_file_count > 0) {
        const auto& r = recent_files[heroIndex()];
        static char heroDetail[200];
        describeRecent(r, heroDetail, sizeof heroDetail);
        const bool done = recentFinished(r);
        params.hero_eyebrow = done ? "WATCHED" : (r.last_pos > 1.0) ? "CONTINUE WATCHING"
                            : episodeInfo(r).is ? "UP NEXT" : "START WATCHING";
        static char heroTitle[160];
        params.hero_title = recentDisplayTitle(r, heroTitle, sizeof heroTitle);
        params.hero_detail = heroDetail;
        static char heroOverview[256];
        std::snprintf(heroOverview, sizeof heroOverview, "%s", r.overview);
        params.hero_overview = heroOverview[0] ? heroOverview : nullptr;
        params.hero_action = done ? "PLAY AGAIN" : (r.last_pos > 1.0) ? "RESUME" : "PLAY";
        if (r.duration > 1.0) {
            params.hero_progress = static_cast<int>((r.last_pos / r.duration) * 1000.0);
        } else {
            params.hero_progress = -1;
        }
        if (m_resumeBusy) {
            /* The stream is being looked up: say so where the eye already is. */
            params.hero_eyebrow = "OPENING";
            params.hero_title = m_resumeTitle.c_str();
            params.hero_detail = "Finding the stream - one moment";
            params.hero_overview = nullptr;
            params.hero_action = "OPENING...";
        }
        if (std::strncmp(r.path, "evo://", 6) == 0) {
            int pw = 0, ph = 0;
            if (const uint32_t* poster = providerPoster(r, &pw, &ph)) {
                params.hero_art = poster;
                params.hero_art_w = pw;
                params.hero_art_h = ph;
            }
        } else if (coverService) {
            /*
             * Only pay for the 960x540 extraction once the cursor has settled
             * (see HeroSettleMs). While it is moving, the shelf's own cached
             * 320x180 poster is the backdrop - it is the same aspect and it is
             * already decoded, so a D-pad press costs nothing.
             */
            if (m_heroSettleMs >= HeroSettleMs) {
                coverService->ensureHeroArt(r.path);
            }

            if (coverService->isHeroArtValid() &&
                coverService->getHeroArtPath() == r.path) {
                params.hero_art = coverService->getHeroArtPixels();
                params.hero_art_w = ICoverArtService::HeroWidth;
                params.hero_art_h = ICoverArtService::HeroHeight;
            } else if (const uint32_t* poster = coverService->peekCoverArt(r.path)) {
                params.hero_art = poster;
                params.hero_art_w = ICoverArtService::PosterWidth;
                params.hero_art_h = ICoverArtService::PosterHeight;
            }
        }
    } else {
        if (m_resumeBusy) {
            params.hero_eyebrow = "OPENING";
            params.hero_title = m_resumeTitle.c_str();
            params.hero_detail = "Finding the stream - one moment";
            params.hero_action = "OPENING...";
        } else {
            /* Nothing to continue: no Providers button (the side menu has
             * Providers); the rows below are what to pick from. */
            params.hero_eyebrow = "WELCOME";
            params.hero_title = "Welcome to Kijkr";
            params.hero_detail = "Nothing to continue yet - pick something below or from the menu";
            params.hero_action = nullptr;
        }
        params.hero_progress = -1;
    }

    // Recent shelf
    const int shelfCount = recentCount();
    params.recent_total = shelfCount;
    params.recent_cursor = (!railFocused && m_selectedRow == RowRecent) ? m_selectedCol : -1;
    params.recent_visible = (shelfCount > EVO_RMLUI_TILES) ? EVO_RMLUI_TILES : shelfCount;

    int coverBudget = 1;
    for (int i = 0; i < params.recent_visible; ++i) {
        const auto& r = recent_files[recentIndex(i)];
        static char tileDetail[EVO_RMLUI_TILES][200];
        describeRecent(r, tileDetail[i], sizeof tileDetail[i]);
        static char tileTitle[EVO_RMLUI_TILES][160];
        params.recent[i].title = recentDisplayTitle(r, tileTitle[i], sizeof tileTitle[i]);
        params.recent[i].detail = tileDetail[i];
        params.recent[i].icon_path = "../icons/icon_recent_files.png";
        if (r.duration > 1.0) {
            params.recent[i].progress = static_cast<int>((r.last_pos / r.duration) * 1000.0);
        } else {
            params.recent[i].progress = -1;
        }

        const uint32_t* art = nullptr;
        int artW = ICoverArtService::PosterWidth;
        int artH = ICoverArtService::PosterHeight;
        if (std::strncmp(r.path, "evo://", 6) == 0) {
            int pw = 0, ph = 0;
            art = providerPoster(r, &pw, &ph);
            if (art) { artW = pw; artH = ph; }
        } else if (coverService) {
            art = coverService->peekCoverArt(r.path);
            bool tried = coverService->hasTriedCoverArt(r.path);
            if (!art && !tried && coverBudget > 0) {
                art = coverService->getCoverArt(r.path, false);
                coverBudget--;
            }
        }
        if (art) {
            params.recent[i].art = art;
        }
        params.recent[i].art_w = artW;
        params.recent[i].art_h = artH;
        params.recent[i].is_focused = (!railFocused && m_selectedRow == RowRecent && m_selectedCol == i);
        params.recent[i].badge = r.rating[0] ? r.rating : nullptr;   /* TMDB's score */
    }

    // Favorites, New films, New series
    static const char* const kShelfTitle[EVO_RMLUI_SHELVES] = {
        "FAVORITES", "TRENDING THIS WEEK", "POPULAR FILMS", "POPULAR SERIES", "NEW FILMS", "NEW SERIES"};
    static char shelfText[EVO_RMLUI_SHELVES][EVO_RMLUI_SHELF_TILES][2][192];
    bool artBudget = true;
    for (int s = 0; s < EVO_RMLUI_SHELVES; ++s) {
        const int row = RowFav + s;
        const std::vector<ShelfItem>& items = shelfItems(s);
        evo_rmlui_launch_shelf_t& out = params.shelves[s];
        out.title = kShelfTitle[s];
        out.portrait = (s == 0) ? 0 : 1;      /* films and series: their posters */
        out.removable = (s == 0) ? 1 : 0;
        out.cursor = (!railFocused && m_selectedRow == row) ? m_selectedCol : -1;
        out.visible = std::min(static_cast<int>(items.size()), EVO_RMLUI_SHELF_TILES);
        for (int i = 0; i < out.visible; ++i) {
            const ShelfItem& it = items[static_cast<size_t>(i)];
            std::snprintf(shelfText[s][i][0], sizeof shelfText[s][i][0], "%s", it.title.c_str());
            std::snprintf(shelfText[s][i][1], sizeof shelfText[s][i][1], "%s", it.detail.c_str());
            evo_rmlui_launch_tile_t& t = out.tiles[i];
            t.title = shelfText[s][i][0];
            t.detail = shelfText[s][i][1];
            t.icon_path = it.folder ? "../icons/icon_folder.png" : (s == 0 ? "../icons/icon_favorites.png"
                                                                           : "../icons/btn_play.png");
            t.progress = it.progress;
            t.is_focused = (!railFocused && m_selectedRow == row && m_selectedCol == i);
            static char badges[EVO_RMLUI_SHELVES][EVO_RMLUI_SHELF_TILES][8];
            std::snprintf(badges[s][i], sizeof badges[s][i], "%s", it.rating.c_str());
            t.badge = badges[s][i][0] ? badges[s][i] : nullptr;
            int w = 0, h = 0;
            const uint32_t* art = nullptr;
            /*
             * Only the posters that can be on screen: the rows next to the
             * cursor, about ten wide around it. Asking for all of Home's
             * hundred-odd posters overran the 24 MB poster cache, which then
             * decoded and threw away posters without end.
             */
            const bool nearRow = std::abs(row - m_selectedRow) <= 1 ||
                                 (m_selectedRow <= RowRecent && row <= RowFav + 1);
            const int from = (m_selectedRow == row) ? std::max(0, m_selectedCol - 4) : 0;
            const bool onScreen = nearRow && i >= from && i < from + 10;
            if (!it.art.empty() && onScreen) {
                art = posterFor(it.key, it.art, &w, &h, &artBudget);
            } else if (onScreen && coverService && it.key.compare(0, 6, "evo://") != 0) {
                art = coverService->peekCoverArt(it.key.c_str());       /* a local favourite */
                w = ICoverArtService::PosterWidth;
                h = ICoverArtService::PosterHeight;
            }
            t.art = art;
            t.art_w = art ? w : 0;
            t.art_h = art ? h : 0;
        }
    }

    // Library shelf (System sections). One table rather than parallel arrays,
    // so a disabled feature drops out of the shelf and out of its activation
    // switch together - see libraryTiles() and EVO_ENABLE_EMBY.
    const LibTile* lib = libraryTiles();
    params.library_visible = 0;   /* the side menu has these */

    for (int i = 0; i < params.library_visible; ++i) {
        params.library[i].title = lib[i].title;
        params.library[i].detail = lib[i].detail;
        params.library[i].icon_path = lib[i].icon;
        params.library[i].progress = -1;
        params.library[i].art = nullptr;
        params.library[i].is_focused = (!railFocused && m_selectedRow == RowLibrary && m_selectedCol == i);
    }

    if (m_detailsOpen) {
        evo_rmlui_launch_details_t& d = params.details;
        d.active = 1;
        d.eyebrow = m_dFolder ? "SERIES" : "FILM";
        d.title = m_dTitle.c_str();
        static char meta[160];
        std::string m = m_dYear;
        if (!m_dRating.empty()) m += (m.empty() ? "" : "  -  ") + m_dRating + " / 10";
        if (m_versions.size() > 1)
            m += (m.empty() ? "" : "  -  ") + std::to_string(m_versions.size()) + " VERSIONS";
        if (m.empty()) m = sourceName(m_dKey);
        std::snprintf(meta, sizeof meta, "%s", m.c_str());
        d.meta = meta;
        d.overview = m_dOverview.c_str();
        const RecentFileEntry* r = recentFor(m_dKey);
        d.progress = m_dFolder ? -1 : progressOf(r);
        int w = 0, h = 0;
        bool budget = true;
        d.art = m_dArt.empty() ? nullptr : posterFor(m_dKey, m_dArt, &w, &h, &budget);
        d.art_w = d.art ? w : 0;
        d.art_h = d.art ? h : 0;
        d.button_count = std::min(static_cast<int>(m_dButtons.size()), EVO_RMLUI_DETAIL_BUTTONS);
        d.focus = m_detailsFocus;
        for (int i = 0; i < d.button_count; ++i) {
            d.buttons[i].label = m_dButtons[static_cast<size_t>(i)].label.c_str();
            d.buttons[i].detail = m_dButtons[static_cast<size_t>(i)].detail.c_str();
        }
    }

    evo_rmlui_update_launch(&params);
    evo_rmlui_render_launch(framebuffer, width, height);

    if (m_confirmClearOpen) {
        static char confirmDetail[128];
        std::snprintf(confirmDetail, sizeof(confirmDetail),
                      "This removes all %d items from Continue Watching. "
                      "Your files are not deleted.", recentCount());
        evo_rmlui_dialog_params_t dlg;
        std::memset(&dlg, 0, sizeof(dlg));
        dlg.eyebrow = "CLEAR ALL";
        dlg.title = "Clear Continue Watching?";
        dlg.detail = confirmDetail;
        dlg.progress_pct = -1.0;
        dlg.action_count = 2;
        dlg.focused_action = m_confirmClearFocus;

        dlg.actions[0].label = "CANCEL";
        dlg.actions[0].icon_path = "../icons/btn_circle.png";
        dlg.actions[0].is_primary = (m_confirmClearFocus == 0) ? 1 : 0;

        dlg.actions[1].label = "CLEAR ALL";
        dlg.actions[1].icon_path = "../icons/btn_cross.png";
        dlg.actions[1].is_primary = (m_confirmClearFocus == 1) ? 1 : 0;

        evo_rmlui_update_dialog(&dlg);
        evo_rmlui_render_dialog(framebuffer, width, height);
    }
}

} // namespace evo
