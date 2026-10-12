#include "evo/screens/SubtitlePickerScreen.hpp"
#include "evo/Application.hpp"
#include "evo_subtitle.h"
#include "evo_subsync.h"
#include "evo_opensubs.h"
#include "evo_rmlui_bridge.h"
#include "evo_feedback.h"
#include "evo_toast.h"

extern "C" {
#include <libavformat/avformat.h>
extern AVFormatContext *play_fmt;
}

#include <cstdio>
#include <cstring>
#include <algorithm>

namespace evo {

SubtitlePickerScreen::SubtitlePickerScreen()
    : StatefulScreen("SubtitlePickerScreen") {
}

void SubtitlePickerScreen::onEnter() {
    StatefulScreen::onEnter();
    m_mode = Mode::Tracks;
    evo_osub_reset();
    refreshTracks();
    m_selectedIndex = m_activeTrackIndex;
    m_scrollOffset = 0;
    if (m_selectedIndex >= kVisibleRows) {
        m_scrollOffset = m_selectedIndex - kVisibleRows + 1;
    }
}

void SubtitlePickerScreen::onExit() {
    StatefulScreen::onExit();
    evo_osub_reset();     /* a reply that lands after we left is dropped */
}

void SubtitlePickerScreen::refreshTracks() {
    m_tracks.clear();
    m_activeTrackIndex = 0;

    // 1. Subtitles OFF
    SubtitleTrackEntry offTrack;
    offTrack.trackId = -2;
    offTrack.label = "SUBTITLES OFF";
    offTrack.detail = "Disable subtitles";
    m_tracks.push_back(std::move(offTrack));

    // 2. External SRT if available
    if (prospero_subtitle_count > 0) {
        SubtitleTrackEntry extTrack;
        extTrack.trackId = -1;
        extTrack.label = "EXTERNAL SRT";
        char cueBuf[32];
        std::snprintf(cueBuf, sizeof(cueBuf), "%d CUES", prospero_subtitle_count);
        extTrack.detail = cueBuf;
        if (prospero_subtitle_enabled && prospero_subtitle_use_external) {
            m_activeTrackIndex = static_cast<int>(m_tracks.size());
        }
        extTrack.isSecondary = prospero_secondary_use_external && prospero_secondary_subtitle_active();
        m_tracks.push_back(std::move(extTrack));
    }

    // 3. Embedded subtitle streams
    if (play_fmt) {
        for (unsigned int i = 0; i < play_fmt->nb_streams; ++i) {
            AVStream* stream = play_fmt->streams[i];
            if (!stream || !stream->codecpar || stream->codecpar->codec_type != AVMEDIA_TYPE_SUBTITLE) {
                continue;
            }

            SubtitleTrackEntry streamTrack;
            streamTrack.trackId = static_cast<int>(i);

            const AVDictionaryEntry* lang = av_dict_get(stream->metadata, "language", nullptr, 0);
            const AVDictionaryEntry* title = av_dict_get(stream->metadata, "title", nullptr, 0);

            std::string label = (lang && lang->value) ? lang->value : "Track";
            if (title && title->value) {
                label += " - ";
                label += title->value;
            }

            streamTrack.label = label;
            streamTrack.detail = "Embedded Stream";
            streamTrack.isSecondary =
                prospero_secondary_subtitle_stream_index == static_cast<int>(i);

            if (prospero_subtitle_enabled && !prospero_subtitle_use_external &&
                prospero_embedded_subtitle_stream_index == static_cast<int>(i)) {
                m_activeTrackIndex = static_cast<int>(m_tracks.size());
            }

            m_tracks.push_back(std::move(streamTrack));
        }
    }

    // AUTO-SYNC (#102): matches the active track - external SRT or an
    // embedded text track - to the audio. Second row, so a long track list
    // never scrolls it away. Its detail is live (progress); render() fills it.
    if (m_tracks.size() > 1) {
        SubtitleTrackEntry syncRow;
        syncRow.trackId = kAutoSyncTrack;
        syncRow.label = "AUTO-SYNC";
        m_tracks.insert(m_tracks.begin() + 1, std::move(syncRow));
        if (m_activeTrackIndex >= 1) {
            ++m_activeTrackIndex;
        }
    }

    // SEARCH SUBTITLE: fetch one from OpenSubtitles. Right under AUTO-SYNC (or under
    // OFF when there is none), so a long track list never scrolls it away.
    {
        SubtitleTrackEntry searchRow;
        searchRow.trackId = kSearchTrack;
        searchRow.label = "SEARCH SUBTITLE";
        searchRow.detail = std::string("OPENSUBTITLES - ") + evo_osub_lang_label(evo_osub_lang_get());
        const size_t pos = (m_tracks.size() > 1 && m_tracks[1].trackId == kAutoSyncTrack) ? 2 : 1;
        m_tracks.insert(m_tracks.begin() + static_cast<long>(pos), std::move(searchRow));
        if (m_activeTrackIndex >= static_cast<int>(pos)) {
            ++m_activeTrackIndex;
        }
    }

    if (!prospero_subtitle_enabled) {
        m_activeTrackIndex = 0;
    }
}

void SubtitlePickerScreen::goBackToPlayer() {
    if (auto sm = Application::getInstance().getScreenManager()) {
        if (!sm->navigateBack()) {
            sm->navigateTo(ScreenId::Player);
        }
    }
}

void SubtitlePickerScreen::startSearch() {
    if (!prospero_opensub_key()[0]) {
        evo_feedback(EVO_FB_ERROR);
        toast("OPENSUBTITLES", "Set the API key in Settings > Subtitles first");
        return;
    }
    if (!evo_osub_user()[0] || !evo_osub_has_password()) {
        evo_feedback(EVO_FB_ERROR);
        toast("OPENSUBTITLES", "Set the username and password in Settings > Subtitles");
        return;
    }

    std::string path, title;
    if (auto pb = Application::getInstance().getPlaybackController()) {
        path = pb->getCurrentFilePath();
        title = pb->getDisplayTitle();
    }
    evo_osub_search_start(path.c_str(), title.c_str());
    if (evo_osub_state() != OSUB_WORKING) {
        evo_feedback(EVO_FB_ERROR);
        toast("OPENSUBTITLES", "Still busy - try again in a moment");
        return;
    }

    evo_feedback(EVO_FB_CONFIRM);
    m_mode = Mode::Searching;
    m_tracks.clear();
    SubtitleTrackEntry status;
    status.trackId = kStatusTrack;
    status.label = "SEARCHING...";
    m_tracks.push_back(std::move(status));
    m_selectedIndex = 0;
    m_scrollOffset = 0;
    m_activeTrackIndex = -1;
}

void SubtitlePickerScreen::showResults() {
    m_mode = Mode::Results;
    m_tracks.clear();
    SubtitleTrackEntry back;
    back.trackId = kBackTrack;
    back.label = "< BACK TO TRACKS";
    m_tracks.push_back(std::move(back));

    const int count = evo_osub_result_count();
    for (int i = 0; i < count; ++i) {
        char label[160], detail[96];
        evo_osub_result_text(i, label, sizeof label, detail, sizeof detail);
        SubtitleTrackEntry entry;
        entry.trackId = kResultBase + i;
        entry.label = label;
        entry.detail = detail;
        m_tracks.push_back(std::move(entry));
    }
    m_selectedIndex = count > 0 ? 1 : 0;
    m_scrollOffset = 0;
    m_activeTrackIndex = -1;
}

void SubtitlePickerScreen::leaveOnline() {
    evo_osub_reset();
    m_mode = Mode::Tracks;
    refreshTracks();
    m_selectedIndex = 0;
    for (size_t i = 0; i < m_tracks.size(); ++i) {
        if (m_tracks[i].trackId == kSearchTrack) {
            m_selectedIndex = static_cast<int>(i);
            break;
        }
    }
    m_scrollOffset = 0;
    if (m_selectedIndex >= kVisibleRows) {
        m_scrollOffset = m_selectedIndex - kVisibleRows + 1;
    }
}

void SubtitlePickerScreen::activateOnline() {
    if (m_selectedIndex < 0 || m_selectedIndex >= static_cast<int>(m_tracks.size())) {
        return;
    }
    const int id = m_tracks[m_selectedIndex].trackId;
    if (m_mode == Mode::Results) {
        if (id == kBackTrack) {
            evo_feedback(EVO_FB_CANCEL);
            leaveOnline();
        } else if (id >= kResultBase) {
            evo_feedback(EVO_FB_CONFIRM);
            evo_osub_download_start(id - kResultBase);
            if (evo_osub_state() == OSUB_WORKING) {
                m_mode = Mode::Downloading;
                m_tracks.clear();
                SubtitleTrackEntry status;
                status.trackId = kStatusTrack;
                status.label = "DOWNLOADING...";
                m_tracks.push_back(std::move(status));
                m_selectedIndex = 0;
                m_scrollOffset = 0;
            } else {
                toast("OPENSUBTITLES", "Still busy - try again in a moment");
            }
        }
        return;
    }
    evo_feedback(EVO_FB_ERROR);   // a request is running: nothing to pick yet
}

/* Called every frame while an online request runs. */
void SubtitlePickerScreen::pumpOnline() {
    const evo_osub_state_t st = evo_osub_state();

    if (m_mode == Mode::Searching) {
        if (st == OSUB_RESULTS) {
            showResults();
        } else if (st == OSUB_FAILED) {
            toast("OPENSUBTITLES", evo_osub_message());
            evo_feedback(EVO_FB_ERROR);
            leaveOnline();
        }
    } else if (m_mode == Mode::Downloading) {
        if (st == OSUB_DONE) {
            const int cues = prospero_subtitle_use_srt_file(evo_osub_file());
            if (cues > 0) {
                char msg[64];
                std::snprintf(msg, sizeof msg, "%d cues loaded", cues);
                toast("SUBTITLES", msg);
                evo_feedback(EVO_FB_CONFIRM);
                evo_osub_reset();
                goBackToPlayer();
            } else {
                toast("OPENSUBTITLES", "That file had no usable subtitles");
                evo_feedback(EVO_FB_ERROR);
                showResults();
            }
        } else if (st == OSUB_FAILED) {
            toast("OPENSUBTITLES", evo_osub_message());
            evo_feedback(EVO_FB_ERROR);
            showResults();
        }
    }
}

void SubtitlePickerScreen::navigate(int delta) {
    if (m_tracks.empty()) return;
    int count = static_cast<int>(m_tracks.size());
    int next = m_selectedIndex + delta;
    if (next < 0) {
        m_selectedIndex = 0;
        evo_feedback(EVO_FB_BOUNDARY);
        return;
    } else if (next >= count) {
        m_selectedIndex = count - 1;
        evo_feedback(EVO_FB_BOUNDARY);
        return;
    }
    m_selectedIndex = next;

    if (m_selectedIndex >= m_scrollOffset + kVisibleRows) {
        m_scrollOffset = m_selectedIndex - kVisibleRows + 1;
    } else if (m_selectedIndex < m_scrollOffset) {
        m_scrollOffset = m_selectedIndex;
    }

    evo_feedback(EVO_FB_MOVE);
}

void SubtitlePickerScreen::activateSelection() {
    if (m_selectedIndex < 0 || m_selectedIndex >= static_cast<int>(m_tracks.size())) {
        return;
    }

    if (m_mode != Mode::Tracks) {
        activateOnline();
        return;
    }

    int trackId = m_tracks[m_selectedIndex].trackId;

    if (trackId == kSearchTrack) {
        startSearch();
        return;
    }

    if (trackId == kAutoSyncTrack) {
        // Start, or cancel a run. Stay on the picker so the progress shows;
        // the result is applied by prospero_subtitle_autosync_pump().
        if (!prospero_subtitle_autosync_running() && !prospero_subtitle_autosync_available()) {
            evo_feedback(EVO_FB_ERROR);
            return;
        }
        evo_feedback(EVO_FB_CONFIRM);
        prospero_subtitle_autosync_toggle();
        return;
    }

    evo_feedback(EVO_FB_CONFIRM);
    if (m_selectedIndex != m_activeTrackIndex) {
        // A run measures the active track; switching away ends it.
        evo_subsync_cancel();
    }

    if (trackId == -2) {
        // Off
        prospero_subtitle_enabled = 0;
        toast("SUBTITLES", "OFF");
    } else if (!prospero_subtitle_select_primary(trackId)) {
        evo_feedback(EVO_FB_ERROR);
        return;
    } else if (trackId == -1) {
        toast("SUBTITLES", "EXTERNAL SRT");
    }
    // An embedded track toasts its own language and codec as it opens.

    if (auto sm = Application::getInstance().getScreenManager()) {
        if (!sm->navigateBack()) {
            sm->navigateTo(ScreenId::Player);
        }
    }
}

// #110: Square makes the focused track the secondary (the second line of
// dialogue), or clears it when the track already is. Unlike Cross this stays
// on the picker, so the [SECONDARY] tag is seen moving.
void SubtitlePickerScreen::activateSecondary() {
    if (m_selectedIndex < 0 || m_selectedIndex >= static_cast<int>(m_tracks.size())) {
        return;
    }

    const SubtitleTrackEntry& entry = m_tracks[m_selectedIndex];

    if (m_mode != Mode::Tracks || entry.trackId == kAutoSyncTrack || entry.trackId == kSearchTrack) {
        evo_feedback(EVO_FB_ERROR);
        return;
    }

    if (entry.trackId == -2 || entry.isSecondary) {
        // The OFF row, or the secondary itself: clear the second line.
        if (!prospero_secondary_subtitle_active()) {
            evo_feedback(EVO_FB_ERROR);
            return;
        }
        prospero_secondary_subtitle_select(PROSPERO_SECONDARY_NONE);
    } else if (!prospero_secondary_subtitle_select(entry.trackId)) {
        evo_feedback(EVO_FB_ERROR);     // it toasted why
        return;
    }

    evo_feedback(EVO_FB_CONFIRM);
    refreshTracks();
}

void SubtitlePickerScreen::cycleSize() {
    /*
     * prospero_subtitle_face is 1=SMALL, 2=MEDIUM, 3=LARGE - see its
     * declaration in evo_subtitle.c, and the classes the player applies:
     * subtitle_face == 1/2/3 -> sub-small/sub-medium/sub-large.
     *
     * This cycled with (face + 1) % 3, which produces 0, 1 and 2. So LARGE was
     * unreachable, and face 0 matched no class at all and fell through to
     * #subtitle-box's own font-size - which is 40dp, exactly what sub-medium
     * sets. Two of the three stops therefore rendered identically and the
     * third was the only one that did anything, which is why the size looked
     * like it never changed.
     */
    if (prospero_subtitle_face < 1 || prospero_subtitle_face > 3)
        prospero_subtitle_face = 2;              /* MEDIUM, the documented default */
    prospero_subtitle_face = (prospero_subtitle_face % 3) + 1;   /* 1 -> 2 -> 3 -> 1 */

    const char* names[] = { "SMALL", "MEDIUM", "LARGE" };
    toast("SUBTITLE SIZE", names[prospero_subtitle_face - 1]);
}

bool SubtitlePickerScreen::handleInput(uint32_t pressed, uint32_t held, uint32_t released) {
    (void)held;
    (void)released;

    if (pressed & PadButtons::Up) {
        navigate(-1);
        return true;
    }
    if (pressed & PadButtons::Down) {
        navigate(1);
        return true;
    }
    if (pressed & PadButtons::Cross) {
        activateSelection();
        return true;
    }
    if (pressed & PadButtons::Triangle) {
        cycleSize();
        return true;
    }
    if (pressed & PadButtons::Square) {
        activateSecondary();
        return true;
    }
    if (m_mode != Mode::Tracks && (pressed & (PadButtons::L1 | PadButtons::R1 | PadButtons::L2 | PadButtons::R2 |
                                              PadButtons::Left | PadButtons::Right |
                                              PadButtons::TouchPadLeft | PadButtons::TouchPadRight |
                                              PadButtons::Square))) {
        return true;
    }
    if (pressed & (PadButtons::L1 | PadButtons::R1)) {
        // The secondary track keeps its own sync (#110).
        if (!prospero_secondary_subtitle_active()) {
            evo_feedback(EVO_FB_ERROR);
            return true;
        }
        prospero_secondary_nudge_delay((pressed & PadButtons::R1) ? +100 : -100);
        evo_feedback(EVO_FB_MOVE);
        return true;
    }
    if (pressed & (PadButtons::Left | PadButtons::TouchPadLeft | PadButtons::L2)) {
        prospero_subtitle_nudge_delay(-100);
        evo_feedback(EVO_FB_MOVE);
        return true;
    }
    if (pressed & (PadButtons::Right | PadButtons::TouchPadRight | PadButtons::R2)) {   /* R2 pairs with L2 */
        prospero_subtitle_nudge_delay(+100);
        evo_feedback(EVO_FB_MOVE);
        return true;
    }
    if (pressed & PadButtons::Circle) {
        evo_feedback(EVO_FB_CANCEL);
        if (m_mode != Mode::Tracks) {
            leaveOnline();        // back to the tracks, not out of the picker
            return true;
        }
        if (auto sm = Application::getInstance().getScreenManager()) {
            if (!sm->navigateBack()) {
                sm->navigateTo(ScreenId::Player);
            }
        }
        return true;
    }

    return false;
}

void SubtitlePickerScreen::update(double deltaMs) {
    StatefulScreen::update(deltaMs);
    if (m_mode == Mode::Searching || m_mode == Mode::Downloading) {
        pumpOnline();
    }
}

void SubtitlePickerScreen::render(uint32_t* framebuffer, int width, int height) {
    evo_rmlui_subtitles_params_t params;
    std::memset(&params, 0, sizeof(params));

    params.eyebrow = "SUBTITLE CONFIGURATION";
    params.title = (m_mode == Mode::Tracks)  ? "SUBTITLE TRACKS"
                 : (m_mode == Mode::Results) ? "SUBTITLE RESULTS"
                                             : "ONLINE SUBTITLES";
    /* The picker's own preview classes are 0/1/2 (preview-small/medium/large)
     * while prospero_subtitle_face is 1/2/3, so this is the one place the two
     * conventions meet and the conversion has to be explicit. Both used to be
     * "% 3", which made the label disagree with the size actually applied. */
    const int size_idx = (prospero_subtitle_face >= 1 && prospero_subtitle_face <= 3)
                       ? (prospero_subtitle_face - 1) : 1;
    const char* sizeNames[] = { "SMALL", "MEDIUM", "LARGE" };
    params.size_str = sizeNames[size_idx];

    static char s_syncBuf[64];
    if (prospero_subtitle_delay_ms == 0) {
        std::snprintf(s_syncBuf, sizeof(s_syncBuf), "SYNC: 0 ms");
    } else {
        std::snprintf(s_syncBuf, sizeof(s_syncBuf), "SYNC: %+d ms", prospero_subtitle_delay_ms);
    }
    if (prospero_subtitle_time_scale != 1.0) {
        // A framerate fix from AUTO-SYNC; the nudge still fine-tunes the offset.
        if (const char* ratio = evo_subsync_ratio_label(prospero_subtitle_time_scale)) {
            size_t len = std::strlen(s_syncBuf);
            std::snprintf(s_syncBuf + len, sizeof(s_syncBuf) - len, "  %s", ratio);
        }
    }
    params.sync_str = s_syncBuf;

    // The secondary track's own sync pill, only while there is one (#110).
    static char s_sync2Buf[32];
    if (prospero_subtitle_enabled && prospero_secondary_subtitle_active()) {
        if (prospero_secondary_delay_ms == 0)
            std::snprintf(s_sync2Buf, sizeof(s_sync2Buf), "2ND: 0 ms");
        else
            std::snprintf(s_sync2Buf, sizeof(s_sync2Buf), "2ND: %+d ms", prospero_secondary_delay_ms);
        params.sync2_str = s_sync2Buf;
    } else {
        params.sync2_str = "";
    }

    params.preview_text = "The quick brown fox jumps over the lazy dog";
    params.preview_face = size_idx;

    int total = static_cast<int>(m_tracks.size());
    int rowsToDisplay = std::min(kVisibleRows, std::max(0, total - m_scrollOffset));
    static char s_autoSyncDetail[48];
    params.track_count = rowsToDisplay;
    for (int i = 0; i < rowsToDisplay; ++i) {
        int idx = m_scrollOffset + i;
        params.tracks[i].label = m_tracks[idx].label.c_str();
        params.tracks[i].detail = m_tracks[idx].detail.c_str();
        params.tracks[i].is_focused = (idx == m_selectedIndex);
        params.tracks[i].is_current = (idx == m_activeTrackIndex);
        // [PRIMARY] / [SECONDARY]: which line of dialogue each track is (#110).
        params.tracks[i].tag = m_tracks[idx].isSecondary ? "SECONDARY"
                             : (idx == m_activeTrackIndex && idx != 0 &&
                                m_tracks[idx].trackId != kAutoSyncTrack) ? "PRIMARY" : "";
        const int tid = m_tracks[idx].trackId;
        if (tid == kSearchTrack || tid == kBackTrack || tid == kStatusTrack || tid >= kResultBase) {
            params.tracks[i].is_action = 1;      // not a track: no radio check
            params.tracks[i].is_current = 0;
            params.tracks[i].tag = "";
            if (tid == kStatusTrack) {
                static char s_statusDetail[200];
                std::snprintf(s_statusDetail, sizeof s_statusDetail, "%s", evo_osub_message());
                params.tracks[i].detail = s_statusDetail;
                params.tracks[i].is_focused = 0;
            }
        }
        if (m_tracks[idx].trackId == kAutoSyncTrack) {
            params.tracks[i].detail = prospero_subtitle_autosync_detail(s_autoSyncDetail, sizeof(s_autoSyncDetail));
            params.tracks[i].is_current = 0;
            params.tracks[i].is_action = 1;
            params.tracks[i].is_disabled = !prospero_subtitle_autosync_running() &&
                                           !prospero_subtitle_autosync_available();
        }
    }

    evo_rmlui_update_subtitles(&params);
    evo_rmlui_render_subtitles(framebuffer, width, height);
}

} // namespace evo
