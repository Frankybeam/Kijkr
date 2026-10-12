#ifndef EVO_LAUNCH_SCREEN_HPP
#define EVO_LAUNCH_SCREEN_HPP

#include "evo/screens/StatefulScreen.hpp"
#include "evo/fsm/StateMachine.hpp"
#include <string>
#include <vector>

namespace evo {

/**
 * @brief Discrete navigation states on the Home/Launch screen.
 */
enum class LaunchScreenState : int {
    HeroFocused = 0,
    RecentShelf = 1,
    LibraryShelf = 2
};

/**
 * @brief Discrete triggers for Home/Launch navigation transitions.
 */
enum class LaunchScreenEvent : int {
    FocusHero = 0,
    FocusRecent = 1,
    FocusLibrary = 2
};

class LaunchScreen : public StatefulScreen {
public:
    LaunchScreen();
    ~LaunchScreen() override = default;

    // --- IStatefulFeature ---
    IStateMachine* getStateMachine() override { return &m_launchFsm; }
    const IStateMachine* getStateMachine() const override { return &m_launchFsm; }

    LaunchScreenState getLaunchState() const {
        return m_launchFsm.getCurrentState();
    }

    ScreenId getScreenId() const override { return ScreenId::MainMenu; }
    void onEnter() override;
    void onExit() override;
    bool handleInput(uint32_t pressed, uint32_t held, uint32_t released) override;
    void update(double deltaMs) override;
    void render(uint32_t* framebuffer, int width, int height) override;

private:
    void initLaunchStateMachine();
    void navigate(int dx, int dy);
    void activateSelection();
    void removeFocusedRecent();   /* Square: drop the focused Continue Watching entry */
    void removeRecentAt(int index);

    /*
     * Home's rows, top to bottom. A row with nothing in it is skipped by Up and
     * Down. Favorites, Trending, Popular films / series (TMDB, what the portal
     * has) and New films / series (the portal's newest) sit between Continue
     * Watching and the library.
     */
    enum Row { RowHero = 0, RowRecent = 1, RowFav = 2, RowTrending = 3, RowPopFilms = 4, RowPopSeries = 5,
               RowNewFilms = 6, RowNewSeries = 7, RowLibrary = 8, RowCount = 9 };
    int  rowCount(int row) const;
    void keepSelectionValid();
    void activateShelf(int row, int col);
    /* Play a Continue Watching key or a file: a provider item is looked up in
     * the background (OPENING on the hero), a file plays at once. */
    void playKey(const std::string& key, const std::string& title, double pos);
    void goToSeries(int recentIdx);
    void toggleShelfWatched(const std::string& key, const std::string& title, const std::string& art);
    /* The newest films and series of the IPTV portal, refreshed now and then. */
    void refreshNewRows(bool force);
    double m_newRowsAgeMs = 1e12;

    /* OPTIONS menu over Home: a list whose choices depend on the tile. */
    void openOptionsMenu();
    bool handleOptionsInput(uint32_t pressed);
    void runOptionsAction(int action);
    void renderOptionsMenu(uint32_t* framebuffer, int width, int height);
    struct MenuItem { int code; std::string label; std::string detail; };
    std::vector<MenuItem> m_menu;
    std::string m_menuTitle;
    bool m_optionsOpen = false;
    int  m_optionsFocus = 0;
    int  m_optionsTarget = 0;   /* index into recent_files (Continue Watching) */
    int  m_menuRow = 0;         /* the row and tile the menu is about */
    int  m_menuCol = 0;
    std::string m_menuKey, m_menuItemTitle, m_menuArt;
    bool m_menuFolder = false;
    /* Several versions of one title in the portal ("Dune", "|NL| Dune",
     * "Dune 4K"): the menu lists them and the chosen one starts. */
    std::vector<std::pair<std::string, std::string>> m_versions;   /* key, portal title */
    void openVersionMenu(const std::string& title, const std::string& art, bool folder,
                         const std::vector<std::pair<std::string, std::string>>& versions);
    void startKey(const std::string& key, const std::string& title, const std::string& art, bool folder);
    /* TMDB's year, score and description of what is about to start, for
     * Continue Watching (set when a poster is chosen). */
    std::string m_hintYear, m_hintRating, m_hintOverview;

    /* The details page (Cross on a poster). */
    bool m_detailsOpen = false;
    int  m_detailsFocus = 0;
    std::string m_dKey, m_dTitle, m_dArt, m_dYear, m_dRating, m_dOverview;
    bool m_dFolder = false;
    std::vector<MenuItem> m_dButtons;
    void openDetails(int row, int col);
    void buildDetailButtons();
    bool handleDetailsInput(uint32_t pressed);
    void runDetailsAction(int code);

    /* Confirmation before CLEAR ALL. Defaults to CANCEL so a stray Cross is harmless. */
    bool handleConfirmClearInput(uint32_t pressed);
    bool m_confirmClearOpen = false;
    int  m_confirmClearFocus = 0;   /* 0 = CANCEL, 1 = CLEAR ALL */

    /* Home shows Continue Watching in a tidier order than the stored one: what is
     * still in progress (or not started) first, in the order it was watched, and
     * anything already finished last. recentIndex(position) is the index into
     * recent_files of what sits at that position on Home. */
    int  recentIndex(int position) const;

    /* A provider item resumed from here is looked up in the background; Home stays
     * up with an OPENING state and the player opens when the stream is ready. */
    bool        m_resumeBusy = false;
    std::string m_resumeTitle;
    /* The other versions of what is being started: tried in turn when one will not play. */
    std::vector<std::pair<std::string, std::string>> m_retryVersions;
    std::string m_retryArt;
    void        resumeEntry(int recentIdx);

    /* Path the hero backdrop should show for the current cursor position. */
    std::string heroPathForSelection() const;
    int heroIndex() const;

    StateMachine<LaunchScreenState, LaunchScreenEvent> m_launchFsm;
    int m_selectedRow = 0;
    int m_selectedCol = 0;

    /*
     * Hero backdrop resolution is debounced. ensureHeroArt() decodes a video
     * frame synchronously on the render thread, and the hero follows the
     * recent-shelf cursor, so resolving it on every D-pad press put a full
     * demux + decode + scale in the input path. The cached poster stands in
     * (same 16:9, just 320x180) until the cursor has been still long enough
     * for the sharp 960x540 extraction to be worth paying for.
     */
    static constexpr double HeroSettleMs = 260.0;
    std::string m_heroSettlePath;
    double m_heroSettleMs = 0.0;
};

} // namespace evo

#endif // EVO_LAUNCH_SCREEN_HPP
