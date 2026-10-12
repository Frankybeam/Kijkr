#ifndef EVO_SUBTITLE_PICKER_SCREEN_HPP
#define EVO_SUBTITLE_PICKER_SCREEN_HPP

#include "evo/screens/StatefulScreen.hpp"
#include <string>
#include <vector>

namespace evo {

class SubtitlePickerScreen : public StatefulScreen {
public:
    SubtitlePickerScreen();
    ~SubtitlePickerScreen() override = default;

    ScreenId getScreenId() const override { return ScreenId::SubtitlePicker; }
    void onEnter() override;
    void onExit() override;
    bool handleInput(uint32_t pressed, uint32_t held, uint32_t released) override;
    void update(double deltaMs) override;
    void render(uint32_t* framebuffer, int width, int height) override;

private:
    struct SubtitleTrackEntry {
        int trackId = -2; // -2 = Off, -1 = External SRT, -3 = AUTO-SYNC, >= 0 = Stream index
        std::string label;
        std::string detail;
        bool isSecondary = false; // #110: this track is the second line of dialogue
    };

    /* subtitles.rml has six rows; scrolling by any other count lets the focus
     * walk onto rows that are never drawn. */
    static constexpr int kVisibleRows = 6;
    static constexpr int kAutoSyncTrack = -3;
    static constexpr int kSearchTrack = -4;   /* SEARCH SUBTITLE (OpenSubtitles) */
    static constexpr int kBackTrack = -5;     /* first row of the result list */
    static constexpr int kStatusTrack = -6;   /* "SEARCHING...", not selectable */
    static constexpr int kResultBase = 1000;  /* trackId = kResultBase + result index */

    /* The picker shows the tracks, or - after SEARCH SUBTITLE - the progress of an
     * online search and its results on the same six rows. */
    enum class Mode { Tracks, Searching, Results, Downloading };

    void refreshTracks();
    void navigate(int delta);
    void activateSelection();
    void activateSecondary();
    void cycleSize();
    void startSearch();
    void showResults();
    void leaveOnline();
    void activateOnline();
    void pumpOnline();
    void goBackToPlayer();

    std::vector<SubtitleTrackEntry> m_tracks;
    int m_selectedIndex = 0;
    int m_activeTrackIndex = 0;
    int m_scrollOffset = 0;
    Mode m_mode = Mode::Tracks;
};

} // namespace evo

#endif // EVO_SUBTITLE_PICKER_SCREEN_HPP
