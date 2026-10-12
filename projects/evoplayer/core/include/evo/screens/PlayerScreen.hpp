#ifndef EVO_PLAYER_SCREEN_HPP
#define EVO_PLAYER_SCREEN_HPP

#include "evo/screens/StatefulScreen.hpp"
#include "evo/fsm/StateMachine.hpp"

namespace evo {

/**
 * @brief Discrete UI states for the media player presentation layer.
 */
enum class PlayerScreenState : int {
    NormalPlayback = 0,
    OsdVisible = 1,
    Scrubbing = 2,
    StatsOverlay = 3
};

/**
 * @brief Discrete trigger events for the player presenter state machine.
 */
enum class PlayerScreenEvent : int {
    ShowOsd = 0,
    HideOsd = 1,
    StartScrub = 2,
    EndScrub = 3,
    ToggleStats = 4
};

class PlayerScreen : public StatefulScreen {
public:
    PlayerScreen();
    ~PlayerScreen() override = default;

    // --- IStatefulFeature ---
    IStateMachine* getStateMachine() override { return &m_playerFsm; }
    const IStateMachine* getStateMachine() const override { return &m_playerFsm; }

    PlayerScreenState getPlayerState() const {
        return m_playerFsm.getCurrentState();
    }

    ScreenId getScreenId() const override { return ScreenId::Player; }
    void onEnter() override;
    void onExit() override;
    bool handleInput(uint32_t pressed, uint32_t held, uint32_t released) override;
    void update(double deltaMs) override;
    void render(uint32_t* framebuffer, int width, int height) override;

    bool isStatsForNerdsVisible() const { return m_showStatsForNerds; }
    void toggleStatsForNerds();
    bool hasActiveOverlay() const;
    double subtitleClockSeconds() const;

private:
    void initPlayerStateMachine();
    void feedPerformanceHud();
    void zapStalker(int dir);          /* live Stalker channel: previous / next */
    void zapEpisode(int dir);          /* Stalker / Xtream series: previous / next episode */
    int m_zapRetryDir = 0;             /* Xtream: the series is being fetched, try again */
    /* The last minute of an episode: the next one's link is asked for ahead,
     * so Up next / R1 start it without waiting for the portal. */
    std::string m_prefetchFor;         /* the file it was done for */
    void prefetchNextEpisode();
    /* The stream ended early (the connection went): ask for a fresh link and
     * carry on at the same place, three times at most. */
    int m_reconnectTries = 0;
    double m_reconnectAt = 0.0;
    bool m_reconnectGaveUp = false;
    std::string m_reconnectItem;       /* the item the count is for */
    void checkReconnect();
    uint64_t m_zapRetryUntil = 0;
    uint64_t m_zapRetryNext = 0;

    StateMachine<PlayerScreenState, PlayerScreenEvent> m_playerFsm;
    int m_osdVisibilityAlpha = 255;
    uint64_t m_controlsLastUsedMs = 0;
    bool m_stallToastShown = false;   /* "connection lost" is up; say when it is back */
    /* Up next: 0 idle, 1 counting down, 2 done or cancelled for this file. */
    int m_upNextState = 0;
    uint64_t m_upNextStartMs = 0;
    int m_upNextShown = -1;
    std::string m_upNextFor;
    uint64_t m_lastPlayPauseToggleMs = 0;
    bool m_showStatsForNerds = false;
};

/* Sleep timer (Settings -> Interface): the index of the choice made, 0 = off.
 * Counts from the moment it is set; the player stops when it runs out. */
int  sleepTimerChoice();
void setSleepTimerChoice(int index);
const char* sleepTimerLabel(int index);
int  sleepTimerChoiceCount();

} // namespace evo

#endif // EVO_PLAYER_SCREEN_HPP
