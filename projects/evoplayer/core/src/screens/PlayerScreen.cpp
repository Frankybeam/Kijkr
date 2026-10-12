#include "evo/screens/PlayerScreen.hpp"
#include "evo/Application.hpp"
#include "evo_rmlui_bridge.h"
#include "evo_playback.h"
#include "evo_demux.h"
#include "evo_audio_out.h"
#include "evo_subtitle.h"
#include "evo_vdec.h"
#include "evo_perf_monitor.h"
#include "evo_toast.h"
#include "evo_feedback.h"
#include "evo_boot_log.h"
#include "evo_dovi.h"
#include "evo_agc_runtime.h"
#include "evo_hw.h"
#include "pp_playback.h"   /* g_pp_pb.seek_discarding - the LOADING badge */
#include "evo_parallel_io.h"
#include "evo_provider.h"      /* resolving the next channel */
#include "evo_stream_io.h"     /* evo_stream_headers */
#include "provider_stalker.h"  /* channel up / down */
#include "addon_xtream.h"     /* Xtream episodes: previous / next */
#include "evo_title_tag.h"
#include <string>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <sys/time.h>

extern evo_vdec *g_vdec;
extern AVFormatContext *play_fmt;
extern AVCodecContext *audio_ctx;
extern int perf_render_fps;
/* Bridge.cpp - reads the DEBUG OVERLAY setting for both FPS pills. */
int evo_fps_counter_enabled(void);
extern int perf_decode_fps;
extern int detected_audio_rate;
extern int evo_audio_channels;
extern char current_media_path[768];
extern double resume_base_offset_seconds;
void draw_video_frame_to_fb(uint32_t *fb, int x, int y, int max_w, int max_h);
extern pp_playback g_pp_pb;
/* Bridge.cpp - raised at open and again whenever the demux queue runs dry on a
 * network source. Both decode threads are parked for as long as it is set. */
extern volatile int pb_prebuffer_hold;
}

#include <cstdio>
#include <cstring>
#include <cmath>

namespace evo {

static uint64_t NowMs() {
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    return static_cast<uint64_t>(tv.tv_sec) * 1000ULL + static_cast<uint64_t>(tv.tv_usec / 1000ULL);
}

/*
 * The network reader is stuck waiting on the server and the video queue has
 * run dry. The demuxer is blocked inside that read, so the rebuffer hold never
 * arms and nothing else would say why the picture stopped: on hardware
 * 2026-10-07 a server outage showed a frozen frame, silently, for a minute.
 */
static bool readerStalled() {
    evo_pio_stats_t st;
    evo_pio_get_stats(&st);
    return st.active && st.stall_ms > 1500 && packet_queue_count(&video_packet_queue) < 3;
}

PlayerScreen::PlayerScreen()
    : StatefulScreen("PlayerScreen")
    , m_playerFsm(PlayerScreenState::NormalPlayback, "PlayerPresenterFSM")
{
    initPlayerStateMachine();
}

/* ---- sleep timer ------------------------------------------------------- */
namespace {
const int kSleepMinutes[] = {0, 15, 30, 45, 60, 90, 120};
const char* const kSleepLabels[] = {"OFF", "15 MIN", "30 MIN", "45 MIN", "60 MIN", "90 MIN", "120 MIN"};
int      g_sleepChoice = 0;
uint64_t g_sleepDeadlineMs = 0;
bool     g_sleepWarned = false;
}

int sleepTimerChoice() { return g_sleepChoice; }
int sleepTimerChoiceCount() { return static_cast<int>(sizeof kSleepMinutes / sizeof kSleepMinutes[0]); }
const char* sleepTimerLabel(int index) {
    return (index >= 0 && index < sleepTimerChoiceCount()) ? kSleepLabels[index] : kSleepLabels[0];
}
void setSleepTimerChoice(int index) {
    if (index < 0 || index >= sleepTimerChoiceCount()) index = 0;
    g_sleepChoice = index;
    g_sleepWarned = false;
    g_sleepDeadlineMs = index ? NowMs() + static_cast<uint64_t>(kSleepMinutes[index]) * 60000ULL : 0;
    toast("SLEEP TIMER", index ? kSleepLabels[index] : "OFF");
}

void PlayerScreen::initPlayerStateMachine() {
    m_playerFsm
        .addState(PlayerScreenState::NormalPlayback, "NormalPlayback")
        .addState(PlayerScreenState::OsdVisible, "OsdVisible")
        .addState(PlayerScreenState::Scrubbing, "Scrubbing")
        .addState(PlayerScreenState::StatsOverlay, "StatsOverlay");

    m_playerFsm
        .addTransition(PlayerScreenState::NormalPlayback, PlayerScreenEvent::ShowOsd, PlayerScreenState::OsdVisible)
        .addTransition(PlayerScreenState::NormalPlayback, PlayerScreenEvent::StartScrub, PlayerScreenState::Scrubbing)
        .addTransition(PlayerScreenState::NormalPlayback, PlayerScreenEvent::ToggleStats, PlayerScreenState::StatsOverlay)
        .addTransition(PlayerScreenState::OsdVisible, PlayerScreenEvent::HideOsd, PlayerScreenState::NormalPlayback)
        .addTransition(PlayerScreenState::OsdVisible, PlayerScreenEvent::StartScrub, PlayerScreenState::Scrubbing)
        .addTransition(PlayerScreenState::OsdVisible, PlayerScreenEvent::ToggleStats, PlayerScreenState::StatsOverlay)
        .addTransition(PlayerScreenState::Scrubbing, PlayerScreenEvent::EndScrub, PlayerScreenState::OsdVisible)
        .addTransition(PlayerScreenState::StatsOverlay, PlayerScreenEvent::ToggleStats, PlayerScreenState::NormalPlayback)
        .addTransition(PlayerScreenState::StatsOverlay, PlayerScreenEvent::ShowOsd, PlayerScreenState::OsdVisible);
}

void PlayerScreen::toggleStatsForNerds() {
    m_showStatsForNerds = !m_showStatsForNerds;
    m_playerFsm.postEvent(PlayerScreenEvent::ToggleStats);
}

/*
 * The clock subtitles are timed against: the raw audio clock when a real audio
 * track is open, otherwise the video clock, plus the resume offset - never the
 * OSD position, which reports the scrub target while the user is scrubbing and
 * would drag every cue to the scrub head instead of the picture on screen.
 */
double PlayerScreen::subtitleClockSeconds() const {
    double clock = (audio_ctx && audio_handle >= 1) ? (double)audio_clock_seconds
                                                    : evo_pb_video_clock_s();
    double pos = resume_base_offset_seconds + clock;
    return (pos < 0.0) ? 0.0 : pos;
}

bool PlayerScreen::hasActiveOverlay() const {
    if (m_osdVisibilityAlpha > 0 || m_showStatsForNerds) {
        return true;
    }
    if (prospero_subtitle_enabled) {
        auto playback = Application::getInstance().getPlaybackController();
        if (playback && !playback->isMusicMode()) {
            double subPos = prospero_subtitle_position(subtitleClockSeconds());
            if (prospero_subtitle_use_external) {
                const ProsperoSubtitleCue* cue = prospero_subtitle_active_cue(subPos);
                if (cue && cue->text[0]) return true;
            } else {
                char activeSubText[PROSPERO_EMBEDDED_SUBTITLE_TEXT_SIZE] = {0};
                prospero_embedded_subtitle_text_at(subPos, activeSubText, sizeof(activeSubText));
                if (activeSubText[0]) return true;
            }

            // #110: a caption in the secondary line is an overlay too.
            char secondarySubText[PROSPERO_EMBEDDED_SUBTITLE_TEXT_SIZE] = {0};
            prospero_secondary_subtitle_text_at(subtitleClockSeconds(), secondarySubText,
                                                sizeof(secondarySubText));
            if (secondarySubText[0]) return true;
        }
    }
    return false;
}

void PlayerScreen::onEnter() {
    StatefulScreen::onEnter();
    m_osdVisibilityAlpha = 255;
    m_controlsLastUsedMs = NowMs();
    m_playerFsm.postEvent(PlayerScreenEvent::ShowOsd);
}

void PlayerScreen::onExit() {
    StatefulScreen::onExit();
}

namespace {

/*
 * Channel up / down on a live Stalker channel. The next channel is resolved while
 * the current one keeps playing; update() switches once the link is in. The
 * resolve callback only sets these flags - it can land after the screen is gone.
 */
struct ZapState {
    bool resolving = false;
    bool ready = false;
    bool failed = false;
    int  settle = 0;                    /* frames to let the toast show before the open blocks */
    bool live = true;                   /* a channel, or an episode */
    double start_at = 0.0;              /* a reconnect carries on here */
    std::string provider = "stalker";
    std::string item_id, title;
    evo_stream_choice_t choice;
};
ZapState g_zap;

/* The next episode, resolved ahead (the last minute of this one). */
struct Prefetch {
    bool resolving = false;
    bool ready = false;
    std::string provider, item_id, title;
    std::string playing_id, playing_url;   /* put back once it has resolved */
    uint64_t at_ms = 0;
    evo_stream_choice_t choice;
};
Prefetch g_prefetch;

void on_zap_resolved(int ok, const evo_stream_choice_t* choices, int count, void* ud)
{
    (void)ud;
    if (ok && choices && count > 0) {
        g_zap.choice = choices[0];
        g_zap.ready = true;
    } else {
        g_zap.failed = true;
    }
    g_zap.resolving = false;
}

void on_prefetch_resolved(int ok, const evo_stream_choice_t* choices, int count, void* ud)
{
    (void)ud;
    g_prefetch.resolving = false;
    /* Resolving it made it "the episode playing": the one on screen still is. */
    if (g_prefetch.provider == "xtream")
        xtream_note_playing(g_prefetch.playing_id.c_str(), g_prefetch.playing_url.c_str());
    else
        stalker_note_playing(g_prefetch.playing_id.c_str(), g_prefetch.playing_url.c_str());
    if (ok && choices && count > 0) {
        g_prefetch.choice = choices[0];
        g_prefetch.ready = true;
        evo_boot_log("player: next episode ready ahead (%s)", g_prefetch.title.c_str());
    }
}

/* A series episode from Stalker or Xtream is playing (L1 / R1, Up next). */
bool episodePlaying(const std::string& url) {
    return stalker_is_playing_episode(url.c_str()) || xtream_is_playing_episode(url.c_str());
}

} // namespace

void PlayerScreen::zapStalker(int dir) {
    if (g_zap.resolving || g_zap.ready) return;          /* one switch at a time */
    char id[160], title[160];
    if (stalker_zap_neighbor(dir, id, sizeof id, title, sizeof title) != 0) {
        toast("LIVE TV", "NO OTHER CHANNEL TO SWITCH TO");
        return;
    }
    const evo_provider_t* p = evo_provider_find("stalker");
    if (!p || !p->resolve) return;
    g_zap = ZapState();
    g_zap.resolving = true;
    g_zap.item_id = id;
    g_zap.title = title;
    toast("LIVE TV", title);
    if (p->resolve(id, on_zap_resolved, nullptr) != 0) {
        g_zap.resolving = false;
        toast("LIVE TV", "COULD NOT SWITCH CHANNEL");
    }
}

/* L1 / R1 while a series episode plays: the one before / after it. */
void PlayerScreen::zapEpisode(int dir) {
    if (g_zap.resolving || g_zap.ready) return;
    char id[160], title[160];
    auto pb = Application::getInstance().getPlaybackController();
    const bool xtream = pb && xtream_is_playing_episode(pb->getCurrentFilePath().c_str());
    if (pb && !xtream)
        stalker_note_title(pb->getDisplayTitle().c_str());       /* the series' name, after a Home resume */
    const int r = xtream ? xtream_episode_neighbor(dir, id, sizeof id, title, sizeof title)
                         : stalker_episode_neighbor(dir, id, sizeof id, title, sizeof title);
    if (r == -3) {
        /* Xtream after a resume from Home: the series is being read. */
        if (m_zapRetryDir != dir) {
            toast("SERIES", "FINDING THE NEXT EPISODE...");
            m_zapRetryDir = dir;
            m_zapRetryUntil = NowMs() + 10000;
        }
        m_zapRetryNext = NowMs() + 500;
        return;
    }
    m_zapRetryDir = 0;
    if (r == -2) {
        toast("SERIES", "OPEN THE NEXT SEASON ONCE SO ITS EPISODES ARE LOADED");
        return;
    }
    if (r != 0) {
        toast("SERIES", dir > 0 ? "THAT WAS THE LAST EPISODE" : "THIS IS THE FIRST EPISODE");
        return;
    }
    const char* provider = xtream ? "xtream" : "stalker";
    if (dir > 0 && g_prefetch.ready && g_prefetch.item_id == id && NowMs() - g_prefetch.at_ms < 5 * 60 * 1000) {
        /* Resolved ahead in the last minute: no wait for the portal. */
        g_zap = ZapState();
        g_zap.provider = provider;
        g_zap.live = false;
        g_zap.item_id = id;
        g_zap.title = title;
        g_zap.choice = g_prefetch.choice;
        g_zap.ready = true;
        g_prefetch = Prefetch();
        toast("SERIES", title);
        return;
    }
    const evo_provider_t* p = evo_provider_find(provider);
    if (!p || !p->resolve) return;
    g_zap = ZapState();
    g_zap.provider = provider;
    g_zap.live = false;
    g_zap.resolving = true;
    g_zap.item_id = id;
    g_zap.title = title;
    toast("SERIES", title);
    if (p->resolve(id, on_zap_resolved, nullptr) != 0) {
        g_zap.resolving = false;
        toast("SERIES", "COULD NOT START THAT EPISODE");
    }
}

/* The last minute of a Stalker episode: ask for the next one's link now. */
void PlayerScreen::prefetchNextEpisode() {
    auto pb = Application::getInstance().getPlaybackController();
    if (!pb || pb->isLiveSource()) return;
    const std::string& cur = pb->getCurrentFilePath();
    if (cur == m_prefetchFor || !stalker_is_playing_episode(cur.c_str())) return;   /* Xtream needs no wait */
    const double dur = pb->getDurationSeconds(), pos = pb->getPositionSeconds();
    if (dur < 120.0 || pos < dur - 60.0 || g_zap.resolving || g_zap.ready || g_prefetch.resolving) return;
    m_prefetchFor = cur;
    stalker_note_title(pb->getDisplayTitle().c_str());
    char id[160], title[160];
    if (stalker_episode_neighbor(+1, id, sizeof id, title, sizeof title) != 0) return;
    const evo_provider_t* p = evo_provider_find("stalker");
    if (!p || !p->resolve) return;
    g_prefetch = Prefetch();
    g_prefetch.resolving = true;
    g_prefetch.provider = "stalker";
    g_prefetch.item_id = id;
    g_prefetch.title = title;
    g_prefetch.playing_id = pb->getSource().item_id;
    g_prefetch.playing_url = cur;
    g_prefetch.at_ms = NowMs();
    if (p->resolve(id, on_prefetch_resolved, nullptr) != 0) g_prefetch.resolving = false;
}

/* The stream stopped well before its end (or a channel stopped): the
 * connection went and the demuxer's own retries gave up. A fresh link from
 * the portal, at the same place - three times, then it says so. */
void PlayerScreen::checkReconnect() {
    auto pb = Application::getInstance().getPlaybackController();
    if (!pb) return;
    const PlaybackSource& src = pb->getSource();
    const double dur = pb->getDurationSeconds(), pos = pb->getPositionSeconds();
    if (src.item_id != m_reconnectItem) {                        /* something else plays now */
        m_reconnectItem = src.item_id;
        m_reconnectTries = 0;
        m_reconnectGaveUp = false;
        m_reconnectAt = 0.0;
    }
    if (pos > m_reconnectAt + 60.0 && m_reconnectTries > 0) {   /* playing well again */
        m_reconnectTries = 0;
        m_reconnectGaveUp = false;
    }
    if (!src.isProvider() || evo_pb_is_eof() == 0 || g_zap.resolving || g_zap.ready) return;
    const bool early = src.is_live || (dur > 1.0 && pos < dur - 90.0);
    if (!early) return;
    if (m_reconnectTries >= 3) {
        if (!m_reconnectGaveUp) {
            m_reconnectGaveUp = true;
            toast("CONNECTION LOST", "The stream could not be restored - press O to go back");
        }
        return;
    }
    const evo_provider_t* p = evo_provider_find(src.provider.c_str());
    if (!p || !p->resolve) return;
    m_reconnectTries++;
    m_reconnectAt = src.is_live ? 0.0 : pos;
    g_zap = ZapState();
    g_zap.provider = src.provider;
    g_zap.live = src.is_live;
    g_zap.item_id = src.item_id;
    g_zap.title = src.title;
    g_zap.start_at = src.is_live ? 0.0 : std::max(0.0, pos - 5.0);   /* a few seconds back */
    g_zap.resolving = true;
    char msg[96];
    std::snprintf(msg, sizeof msg, "Reconnecting - try %d of 3", m_reconnectTries);
    toast("CONNECTION LOST", msg);
    evo_boot_log("player: stream ended early at %.0fs of %.0fs - reconnect %d", pos, dur, m_reconnectTries);
    if (p->resolve(src.item_id.c_str(), on_zap_resolved, nullptr) != 0) g_zap.resolving = false;
}

bool PlayerScreen::handleInput(uint32_t pressed, uint32_t held, uint32_t released) {
    (void)released;
    auto playback = Application::getInstance().getPlaybackController();
    auto screenMgr = Application::getInstance().getScreenManager();
    if (!playback) return false;

    if (pressed) {
        m_controlsLastUsedMs = NowMs();
    }

    // Scrub hold update
    playback->updateScrubHold(held);

    /* Up next is counting: Cross plays it now, Circle stays on this one. */
    if (m_upNextState == 1 && (pressed & (PadButtons::Cross | PadButtons::Circle))) {
        m_upNextState = 2;
        if (pressed & PadButtons::Cross) {
            evo_feedback(EVO_FB_CONFIRM);
            zapEpisode(1);
        } else {
            evo_feedback(EVO_FB_CANCEL);
            toast("UP NEXT", "CANCELLED");
        }
        return true;
    }

    /* A seek is being lined up (D-pad / L2 / R2): Cross jumps there now
     * instead of pausing, Circle puts the marker back instead of asking to
     * stop. Without the seek, both keep their usual meaning below. */
    if (playback->isScrubbing() && (pressed & (PadButtons::Cross | PadButtons::Circle))) {
        if (pressed & PadButtons::Cross) {
            evo_feedback(EVO_FB_CONFIRM);
            playback->confirmScrub();
        } else {
            evo_feedback(EVO_FB_CANCEL);
            playback->cancelScrub();
            toast("SEEK", "CANCELLED");
        }
        return true;
    }

    if (pressed & PadButtons::Cross) {
        uint64_t now = NowMs();
        if (now - m_lastPlayPauseToggleMs >= 150) {
            m_lastPlayPauseToggleMs = now;
            // Toggle play / pause
            playback->togglePause();
            toast("PLAYBACK", playback->isPaused() ? "PAUSED" : "RESUMED");
        }
        return true;
    }
    if (pressed & PadButtons::Circle) {
        // Stop playback confirmation prompt
        if (screenMgr) {
            screenMgr->navigateTo(ScreenId::ExitConfirm);
        }
        return true;
    }
    if (pressed & PadButtons::Options) {
        toggleStatsForNerds();
        toast("STATS", m_showStatsForNerds ? "ON" : "OFF");
        return true;
    }
    if (pressed & PadButtons::Square) {
        if (screenMgr) {
            screenMgr->navigateTo(ScreenId::MediaInfo);
        }
        return true;
    }
    if (pressed & PadButtons::Triangle) {
        /* Aspect ratio, as it has always been on this screen. */
        playback->cycleViewMode();
        toast("VIEW MODE", playback->getViewMode() == ViewMode::Fit ? "FIT" :
                           playback->getViewMode() == ViewMode::Fill ? "FILL" : "STRETCH");
        return true;
    }
    /* Live TV has no timeline: seek and chapter keys are swallowed, not sent
     * to a demuxer that has nothing behind the live edge. */
    /* A Stalker series episode: L1 / R1 are previous / next episode, not chapters. */
    if ((pressed & (PadButtons::L1 | PadButtons::R1)) && !playback->isLiveSource() &&
        episodePlaying(playback->getCurrentFilePath())) {
        zapEpisode((pressed & PadButtons::R1) ? 1 : -1);
        return true;
    }
    if (playback->isLiveSource() &&
        (pressed & (PadButtons::L1 | PadButtons::R1 | PadButtons::Left | PadButtons::Right |
                    PadButtons::L2 | PadButtons::R2))) {
        /* A Stalker channel: L1 / R1 and Left / Right are channel back /
         * forward - the D-pad is what a TV remote uses, and live TV has no
         * timeline for it to seek. L2 / R2 have nothing to jump over. */
        if ((pressed & (PadButtons::L1 | PadButtons::R1 | PadButtons::Left | PadButtons::Right)) &&
            stalker_is_playing(playback->getCurrentFilePath().c_str()))
            zapStalker((pressed & (PadButtons::R1 | PadButtons::Right)) ? 1 : -1);
        return true;
    }
    if (pressed & PadButtons::L1) {
        playback->jumpChapter(-1);
        return true;
    }
    if (pressed & PadButtons::R1) {
        playback->jumpChapter(1);
        return true;
    }
    if (pressed & PadButtons::Left) {
        playback->moveScrub(-10.0);
        return true;
    }
    if (pressed & PadButtons::Right) {
        playback->moveScrub(10.0);
        return true;
    }
    /*
     * Up toggles subtitles, Down opens the picker - the legacy binding. The
     * picker rather than a next-track cycle because cycling reopens the file
     * on every step, which on a disc rip with thirty tracks means thirty
     * reopens to reach the one you want.
     */
    if (pressed & PadButtons::Up) {
        if (!playback->isScrubbing()) {
            prospero_subtitle_toggle();
            toast("SUBTITLES", prospero_subtitle_enabled ? "ON" : "OFF");
        }
        return true;
    }
    if (pressed & PadButtons::Down) {
        if (!playback->isScrubbing() && screenMgr) {
            screenMgr->navigateTo(ScreenId::SubtitlePicker);
        }
        return true;
    }
    /*
     * L2 / R2: a minute back / forward. They move the same seek marker as the
     * D-pad, so presses add up (R2 R2 Right = +2:10) and land together a
     * moment after the last one. They used to be a mismatched pair - L2 moved
     * the subtitles, R2 opened the audio list.
     */
    if (pressed & (PadButtons::L2 | PadButtons::R2)) {
        playback->moveScrub((pressed & PadButtons::R2) ? 60.0 : -60.0);
        return true;
    }
    /*
     * A touchpad CLICK opens the audio list (it was R2). Checked before the
     * halves below: a click also reports which half was pressed, and it
     * shifted the subtitles every time someone pressed the pad.
     */
    if (pressed & PadButtons::TouchPad) {
        if (!playback->isScrubbing() && screenMgr) {
            screenMgr->navigateTo(ScreenId::AudioTrackPicker);
        }
        return true;
    }
    /*
     * Subtitle delay sync, by SWIPING the touchpad:
     * - swipe left:  -100 ms (earlier)
     * - swipe right: +100 ms (later)
     */
    if (pressed & PadButtons::TouchPadLeft) {
        if (!playback->isScrubbing()) {
            prospero_subtitle_nudge_delay(-100);
            evo_feedback(EVO_FB_MOVE);
            m_controlsLastUsedMs = NowMs();
        }
        return true;
    }
    if (pressed & PadButtons::TouchPadRight) {
        if (!playback->isScrubbing()) {
            prospero_subtitle_nudge_delay(+100);
            evo_feedback(EVO_FB_MOVE);
            m_controlsLastUsedMs = NowMs();
        }
        return true;
    }

    return false;
}

void PlayerScreen::update(double deltaMs) {
    StatefulScreen::update(deltaMs);
    m_playerFsm.update(deltaMs);

    auto playback = Application::getInstance().getPlaybackController();
    if (!playback) return;

    playback->tickScrubAutoCommit();

    prefetchNextEpisode();
    checkReconnect();
    if (m_zapRetryDir != 0) {                            /* Xtream: waiting for the series */
        if (NowMs() > m_zapRetryUntil) {
            m_zapRetryDir = 0;
            toast("SERIES", "THE NEXT EPISODE COULD NOT BE FOUND");
        } else if (NowMs() >= m_zapRetryNext) {
            zapEpisode(m_zapRetryDir);
        }
    }
    if (g_zap.failed) {
        g_zap.failed = false;
        toast(g_zap.live ? "LIVE TV" : "SERIES",
              g_zap.live ? "THAT CHANNEL IS NOT AVAILABLE" : "THAT EPISODE IS NOT AVAILABLE");
    }
    if (g_zap.ready && ++g_zap.settle >= 3) {            /* the toast is on screen: open the next one */
        g_zap.ready = false;
        g_zap.settle = 0;
        PlaybackSource src;
        src.url      = g_zap.choice.url;
        src.title    = g_zap.title;
        src.provider = g_zap.provider;
        src.item_id  = g_zap.item_id;
        src.is_live  = g_zap.live;
        src.art_url  = g_zap.choice.art_url;   /* the series poster, for Home */
        if (src.art_url.empty())                /* else the one the last episode had */
            src.art_url = playback->getSource().art_url;
        src.year     = g_zap.choice.year;
        src.ep_total = g_zap.choice.ep_total;
        src.overview = g_zap.choice.overview;
        if (src.year.empty()) src.year = playback->getSource().year;
        if (src.overview.empty()) src.overview = playback->getSource().overview;
        std::snprintf(evo_stream_headers, sizeof evo_stream_headers, "%s", g_zap.choice.headers);
        evo_stream_max_connections = g_zap.choice.max_connections;
        evo_stream_user_agent[0] = '\0';
        if (!playback->startPlaybackSource(src, g_zap.start_at)) {
            toast(g_zap.live ? "LIVE TV" : "SERIES",
                  g_zap.live ? "CHANNEL FAILED TO OPEN" : "EPISODE FAILED TO OPEN");
            playback->stopPlayback();
            if (auto sm = Application::getInstance().getScreenManager())
                sm->returnFromPlayback();
            return;
        }
    }

    /* Sleep timer: a minute's warning, then stop and go back. */
    if (g_sleepDeadlineMs) {
        const uint64_t now = NowMs();
        if (now >= g_sleepDeadlineMs) {
            g_sleepDeadlineMs = 0;
            g_sleepChoice = 0;
            playback->stopPlayback();
            toast("SLEEP TIMER", "STOPPED - GOOD NIGHT");
            if (auto sm = Application::getInstance().getScreenManager())
                sm->returnFromPlayback();
            return;
        }
        if (!g_sleepWarned && g_sleepDeadlineMs - now <= 60000ULL) {
            g_sleepWarned = true;
            toast("SLEEP TIMER", "STOPPING IN 1 MINUTE");
        }
    }

    /*
     * Up next. A Stalker episode that has played to its end counts down ten
     * seconds and starts the next one, as R1 would. Cross starts it at once,
     * Circle cancels; a seek back out of the end stops the count. Once per
     * file, so a cancel is not undone a frame later.
     */
    {
        const std::string& cur = playback->getCurrentFilePath();
        if (cur != m_upNextFor) {
            m_upNextFor = cur;
            m_upNextState = 0;
        }
        const bool episode = !playback->isLiveSource() && episodePlaying(cur);
        const bool atEnd = evo_pb_is_eof() != 0;
        if (m_upNextState == 0 && episode && atEnd && !g_zap.resolving && !g_zap.ready) {
            m_upNextState = 1;
            m_upNextStartMs = NowMs();
            m_upNextShown = -1;
        }
        if (m_upNextState == 1) {
            if (!atEnd) {
                m_upNextState = 0;                       /* seeked back in */
            } else {
                const int left = 10 - static_cast<int>((NowMs() - m_upNextStartMs) / 1000);
                if (left <= 0) {
                    m_upNextState = 2;
                    zapEpisode(1);
                } else if (left != m_upNextShown) {
                    m_upNextShown = left;
                    char msg[96];
                    std::snprintf(msg, sizeof msg, "NEXT EPISODE IN %d  -  X NOW  -  O CANCEL", left);
                    toast("UP NEXT", msg);
                }
            }
        }
    }

    /*
     * The decoder gave up (repeated failures, or a hardware call that stopped
     * returning - #39). The engine has already raised its error toast; without
     * this the player sat on a frozen picture with the controls up and nothing
     * to say why. Leave through the same path as STOP so the toast lands on the
     * screen the user returns to.
     */
    if (evo_pb_decode_fatal()) {
        playback->stopPlayback();
        if (auto sm = Application::getInstance().getScreenManager())
            sm->returnFromPlayback();
        return;
    }

    uint64_t now = NowMs();
    /*
     * Hold the OSD up once the clip has finished.
     *
     * Nothing in the list below is true at EOF - the clock simply stops, it is
     * not "paused", and the 3.5s input timer has long expired - so the OSD
     * faded out and the screen was left showing a held last frame with no
     * indication of what had happened or what the controls were. evo_pb_is_eof()
     * stays set for as long as the finished file is open, so the OSD stays up
     * until something actually changes (a seek back clears it).
     */
    const bool ended = (evo_pb_is_eof() != 0);

    /*
     * Ask the playback engine whether it is paused, not the state machine.
     *
     * Those two disagree. A D-pad seek sets `player_paused` directly
     * (evo_demux.c) and so does restoring a subtitle track, neither of which
     * goes through the FSM - so isPaused() reported false while the picture
     * was genuinely stopped, the 3.5s input timer ran out, and the OSD faded
     * away over a paused frame with nothing on screen to say why.
     *
     * The PAUSED badge below deliberately keeps using isPaused(): the engine
     * flag is also raised for the moment a seek is in flight, and a badge that
     * blinked on every seek would be worse than one that is occasionally late.
     */
    const bool reallyPaused = (evo_pb_is_paused() != 0) || playback->isPaused();

    /* Hold the OSD active while video frames are still buffering into the
     * decoder - and while a seek is: on a network stream that is seconds of a
     * still picture that would otherwise read as frozen. */
    /* Say so once when the server stops the picture, and once when it is back.
     * Raised here rather than in the reader, which stalls long before anyone
     * can see it: the read-ahead keeps playing for half a minute. */
    const bool stalled = readerStalled();
    if (stalled && !m_stallToastShown) {
        m_stallToastShown = true;
        toast("CONNECTION LOST", "Can't get data from the server - retrying...");
    } else if (!stalled && m_stallToastShown && !pb_prebuffer_hold) {
        m_stallToastShown = false;
        toast("CONNECTION", "Reconnected - the stream is back");
    }

    const bool isBuffering = !playback->isMusicMode() &&
                             (!video_frame_loaded || g_pp_pb.seek_discarding ||
                              pb_prebuffer_hold || stalled);
    if (isBuffering) {
        m_controlsLastUsedMs = now;
    }

    bool wantsControls = playback->isMusicMode() || ended ||
                         ((now - m_controlsLastUsedMs < 3500) || reallyPaused || m_showStatsForNerds) ||
                         playback->isScrubbing() || isBuffering;

    if (playback->isScrubbing()) {
        m_playerFsm.postEvent(PlayerScreenEvent::StartScrub);
    } else if (wantsControls) {
        m_playerFsm.postEvent(PlayerScreenEvent::ShowOsd);
    } else {
        m_playerFsm.postEvent(PlayerScreenEvent::HideOsd);
    }

    // Smooth OSD fade in/out
    if (wantsControls) {
        if (m_osdVisibilityAlpha < 255) {
            m_osdVisibilityAlpha = std::min(255, m_osdVisibilityAlpha + 42);
        }
    } else {
        if (m_osdVisibilityAlpha > 0) {
            m_osdVisibilityAlpha = std::max(0, m_osdVisibilityAlpha - 28);
        }
    }
}

void PlayerScreen::feedPerformanceHud() {
    static char l_video[192], l_audio[192], l_subs[128];
    static char l_perf[160], l_queues[160], l_clocks[160];

    char vcodec[48] = "NONE", acodec[48] = "NONE", alang[24] = "und";
    int vw = 0, vh = 0, ach = 0, arate = detected_audio_rate;

    if (play_fmt && video_stream_index >= 0 && video_stream_index < (int)play_fmt->nb_streams) {
        AVCodecParameters *cp = play_fmt->streams[video_stream_index]->codecpar;
        if (cp) {
            std::snprintf(vcodec, sizeof(vcodec), "%s", avcodec_get_name(cp->codec_id));
            vw = cp->width;
            vh = cp->height;
        }
    }
    if (play_fmt && audio_stream_index >= 0 && audio_stream_index < (int)play_fmt->nb_streams) {
        AVStream *st = play_fmt->streams[audio_stream_index];
        if (st && st->codecpar) {
            std::snprintf(acodec, sizeof(acodec), "%s", avcodec_get_name(st->codecpar->codec_id));
            ach = st->codecpar->ch_layout.nb_channels;
            arate = st->codecpar->sample_rate;
            const AVDictionaryEntry *lang = av_dict_get(st->metadata, "language", nullptr, 0);
            if (lang && lang->value && lang->value[0]) {
                std::snprintf(alang, sizeof(alang), "%s", lang->value);
            }
        }
    }

    const char *backend = (evo_pb_active_backend() == EVO_VDEC_BACKEND_NATIVE)
                              ? "Hardware (sceVideodec2)" : "Software (FFmpeg)";

    std::snprintf(l_video, sizeof(l_video), "VIDEO  %s  %dx%d  %.3f fps  (%s)",
                  vcodec, vw, vh, evo_pb_video_fps(), backend);
    std::snprintf(l_audio, sizeof(l_audio), "AUDIO  %s  %s  %dch  %d Hz",
                  acodec, alang, ach, arate);
    std::snprintf(l_subs, sizeof(l_subs), "SUBS  %s  /  %s%s",
                  prospero_subtitle_enabled ? "ON" : "OFF",
                  prospero_subtitle_use_external ? "EXTERNAL"
                      : (prospero_embedded_subtitle_stream_index >= 0 ? "EMBEDDED" : "NONE"),
                  (prospero_subtitle_enabled && prospero_secondary_subtitle_active())
                      ? "  +  2ND TRACK" : "");
    std::snprintf(l_perf, sizeof(l_perf), "RENDER %d fps  /  DECODE %d fps  /  UPSCALER %s  /  SMOOTH %s  /  %s",
                  perf_render_fps, perf_decode_fps, evo_agc_upscale_label(),
                  evo_agc_motion_smoothing_label(),
                  evo_hw_model_name());
    std::snprintf(l_queues, sizeof(l_queues), "QUEUES  video %d/96  audio %d/96  pcm %d",
                  packet_queue_count(&video_packet_queue),
                  packet_queue_count(&audio_packet_queue),
                  audio_queue_count);
    std::snprintf(l_clocks, sizeof(l_clocks), "CLOCKS  video %.2f  audio %.2f  delta %.3f",
                  evo_pb_video_clock_s(), audio_clock_seconds,
                  evo_pb_video_clock_s() - audio_clock_seconds);

    evo_perf_snapshot_t s;
    evo_perf_monitor_get(&s);

    evo_perf_hud_t hud;
    std::memset(&hud, 0, sizeof(hud));
    hud.line_video = l_video;
    hud.line_audio = l_audio;
    hud.line_subs = l_subs;
    hud.line_perf = l_perf;
    hud.line_queues = l_queues;
    hud.line_clocks = l_clocks;
    hud.hist_len = EVO_PERF_HIST;
    hud.gpu_hist = s.gpu_hist;
    hud.ram_hist = s.ram_hist;
    hud.cpu_hist = s.cpu_hist;
    hud.gpu_pct = s.gpu_pct;
    hud.gpu_peak_pct = s.gpu_peak_pct;
    hud.ram_mb = s.ram_mb;
    hud.ram_peak_mb = s.ram_peak_mb;
    hud.ram_total_mb = s.ram_total_mb;
    hud.cpu_pct = s.cpu_pct;
    hud.cpu_peak_pct = s.cpu_peak_pct;

    evo_rmlui_update_perf_hud(&hud);
}

void PlayerScreen::render(uint32_t* framebuffer, int width, int height) {
    auto playback = Application::getInstance().getPlaybackController();
    auto metaService = Application::getInstance().getMediaMetadataService();
    if (!playback) return;

    static int s_player_screen_log = 5;
    if (s_player_screen_log > 0) {
        s_player_screen_log--;
        evo_boot_log("PlayerScreen::render fb=%p %dx%d alpha=%d rml_init=%d",
                     (void*)framebuffer, width, height, m_osdVisibilityAlpha,
                     evo_rmlui_is_initialized());
        evo_boot_log_flush();
    }

    /*
     * Both branches only matter to the CPU rasteriser. In AGC mode RmlUi draws
     * through the GPU and never reads this buffer back, so the music-mode fill
     * was 8.3 MB of stores a frame that nothing looked at -
     * draw_video_frame_to_fb has already been an empty function since the GL
     * quad took over.
     */
    if (evo_rmlui_blit_mode()) {
        if (!playback->isMusicMode()) {
            draw_video_frame_to_fb(framebuffer, 0, 0, width, height);
        } else {
            std::fill_n(framebuffer, width * height, 0xFF000000);
        }
    }

    char activeSubText[PROSPERO_EMBEDDED_SUBTITLE_TEXT_SIZE] = {0};
    if (prospero_subtitle_enabled && !playback->isMusicMode()) {
        double subPos = prospero_subtitle_position(subtitleClockSeconds());

        if (prospero_subtitle_use_external) {
            const ProsperoSubtitleCue* cue = prospero_subtitle_active_cue(subPos);
            if (cue && cue->text[0]) {
                std::snprintf(activeSubText, sizeof(activeSubText), "%s", cue->text);
            }
        } else {
            prospero_embedded_subtitle_text_at(subPos, activeSubText, sizeof(activeSubText));
        }
    }

    // #110: the secondary line of dialogue, with its own delay applied.
    char secondarySubText[PROSPERO_EMBEDDED_SUBTITLE_TEXT_SIZE] = {0};
    if (prospero_subtitle_enabled && !playback->isMusicMode()) {
        prospero_secondary_subtitle_text_at(subtitleClockSeconds(), secondarySubText,
                                            sizeof(secondarySubText));
    }

    if (m_osdVisibilityAlpha > 0 && evo_rmlui_is_initialized()) {
        std::string titleStr, metaStr;
        /*
         * #90 / #9: a provider supplies its own title, and deriving one from
         * the source string is what put a URL with a query string on the OSD.
         * cleanMediaTitle takes the last path component and tidies it, which is
         * exactly right for /mnt/usb0/Movie.2019.mkv and exactly wrong for
         * http://host/emby/Videos/abc/stream?api_key=...
         */
        titleStr = playback->getDisplayTitle();
        if (!titleStr.empty()) {
            char clean[256];
            evo_title_strip_tags(titleStr.c_str(), clean, sizeof clean);   /* "MULTI Dune" -> "Dune" */
            titleStr = clean;
        }
        if (titleStr.empty() && metaService) {
            metaService->cleanMediaTitle(playback->getCurrentFilePath(), titleStr, metaStr);
        } else if (playback->isLiveSource()) {
            metaStr = (!video_frame_loaded) ? "BUFFERING..." : "LIVE";
        }

        /*
         * A rebuffer parks both decode threads mid-playback, so the picture
         * freezes on its last frame with the title still on the OSD - which
         * reads as a hung player rather than a short link. Say which it is.
         * This overrides whatever the branches above chose: while the hold is
         * up, nothing else about the stream is the useful thing to show.
         */
        if (readerStalled())
            metaStr = "WAITING FOR THE SERVER...";
        else if (pb_prebuffer_hold)
            metaStr = "BUFFERING...";

        evo_playback_osd_params_t p;
        std::memset(&p, 0, sizeof(p));
        p.title = titleStr.empty() ? "Video Playback" : titleStr.c_str();
        p.metadata = metaStr.c_str();
        /*
         * Derived from the open stream, not assumed. These were hardcoded to
         * "1080p FHD" / "H.264 / AVC" / "60 FPS", so every file reported the
         * same thing whatever it actually was.
         */
        static char resBuf[32], codecBuf[32], fpsBuf[16];
        const char* hdrBadge = "";
        resBuf[0] = codecBuf[0] = fpsBuf[0] = '\0';

        if (play_fmt && video_stream_index >= 0 &&
            video_stream_index < static_cast<int>(play_fmt->nb_streams)) {
            AVCodecParameters* cp = play_fmt->streams[video_stream_index]->codecpar;
            if (cp) {
                int vh = cp->height, vw = cp->width;
                if (vh >= 2000)      std::snprintf(resBuf, sizeof(resBuf), "4K UHD");
                else if (vh >= 1400) std::snprintf(resBuf, sizeof(resBuf), "1440p QHD");
                else if (vh >= 1000) std::snprintf(resBuf, sizeof(resBuf), "1080p FHD");
                else if (vh >= 700)  std::snprintf(resBuf, sizeof(resBuf), "720p HD");
                else if (vh > 0)     std::snprintf(resBuf, sizeof(resBuf), "%dx%d", vw, vh);

                const char* cn = avcodec_get_name(cp->codec_id);
                switch (cp->codec_id) {
                    case AV_CODEC_ID_H264: cn = "H.264 / AVC";  break;
                    case AV_CODEC_ID_HEVC: cn = "H.265 / HEVC"; break;
                    case AV_CODEC_ID_VP9:  cn = "VP9";          break;
                    case AV_CODEC_ID_AV1:  cn = "AV1";          break;
                    default: break;
                }
                std::snprintf(codecBuf, sizeof(codecBuf), "%s", cn);

                if (evo_dovi_stream_profile(cp) > 0)          hdrBadge = "DOLBY VISION";
                else if (cp->color_trc == AVCOL_TRC_SMPTE2084) hdrBadge = "HDR10";
                else if (cp->color_trc == AVCOL_TRC_ARIB_STD_B67) hdrBadge = "HLG";
            }
        }

        double fps = evo_pb_video_fps();
        if (fps > 1.0)
            std::snprintf(fpsBuf, sizeof(fpsBuf), "%d FPS", static_cast<int>(fps + 0.5));

        p.res_badge   = resBuf[0]   ? resBuf   : "";
        p.hdr_badge   = hdrBadge;
        p.codec_badge = codecBuf[0] ? codecBuf : "";
        p.fps_badge   = fpsBuf[0]   ? fpsBuf   : "";
        /*
         * The two footer labels and the audio badge all told the same lie on
         * every file.
         *
         * `audio_track` and `sub_track` were never assigned at all - the
         * params are memset, so the bridge saw empty strings and printed its
         * fallbacks: "AUDIO: Stereo" over a DD+5.1 ATMOS track, and
         * "SUBS: None" on a file whose Media Info panel said subtitles were
         * Active. The badge had its own version of the same bug, treating
         * anything that was not 8 channels as stereo, so 5.1 read as STEREO.
         */
        static char audioTrackBuf[80], subTrackBuf[80];
        audioTrackBuf[0] = subTrackBuf[0] = '\0';
        const char* chLabel = "";

        if (play_fmt && audio_stream_index >= 0 &&
            audio_stream_index < static_cast<int>(play_fmt->nb_streams)) {
            AVStream* ast = play_fmt->streams[audio_stream_index];
            if (ast && ast->codecpar) {
                const int ch = ast->codecpar->ch_layout.nb_channels;
                chLabel = (ch >= 8) ? "7.1" : (ch >= 6) ? "5.1"
                        : (ch == 2) ? "Stereo" : (ch == 1) ? "Mono" : "";
                const char* an = avcodec_get_name(ast->codecpar->codec_id);
                if (chLabel[0] && an && an[0])
                    std::snprintf(audioTrackBuf, sizeof(audioTrackBuf), "%s %s", an, chLabel);
                else if (an && an[0])
                    std::snprintf(audioTrackBuf, sizeof(audioTrackBuf), "%s", an);
            }
        }

        if (prospero_subtitle_enabled) {
            if (prospero_subtitle_use_external) {
                std::snprintf(subTrackBuf, sizeof(subTrackBuf), "External SRT");
            } else if (play_fmt && prospero_embedded_subtitle_stream_index >= 0 &&
                       prospero_embedded_subtitle_stream_index <
                           static_cast<int>(play_fmt->nb_streams)) {
                AVStream* sst = play_fmt->streams[prospero_embedded_subtitle_stream_index];
                const AVDictionaryEntry* lang =
                    sst ? av_dict_get(sst->metadata, "language", nullptr, 0) : nullptr;
                std::snprintf(subTrackBuf, sizeof(subTrackBuf), "%s",
                              (lang && lang->value && lang->value[0]) ? lang->value : "On");
            }
            /* Enabled but nothing to show (no external file, no embedded
             * track): leave it empty, so the OSD says None - and hides SUBS
             * and SYNC on live TV - instead of claiming subtitles are On. */
        }

        // #110: name the secondary track next to the primary's.
        if (prospero_subtitle_enabled && prospero_secondary_subtitle_active()) {
            const char* second = "SRT";
            if (!prospero_secondary_use_external && play_fmt &&
                prospero_secondary_subtitle_stream_index >= 0 &&
                prospero_secondary_subtitle_stream_index < static_cast<int>(play_fmt->nb_streams)) {
                AVStream* s2 = play_fmt->streams[prospero_secondary_subtitle_stream_index];
                const AVDictionaryEntry* l2 =
                    s2 ? av_dict_get(s2->metadata, "language", nullptr, 0) : nullptr;
                second = (l2 && l2->value && l2->value[0]) ? l2->value : "2nd";
            }
            const size_t used = std::strlen(subTrackBuf);
            std::snprintf(subTrackBuf + used, sizeof(subTrackBuf) - used,
                          used ? " + %s" : "+ %s", second);
        }

        // A stream with several video qualities (an HLS master): show the one
        // playing beside the audio, so R2 (Quality & Audio) is discoverable.
        {
            const auto variants = playback->getVideoVariants();
            if (variants.size() > 1) {
                const int activeVideo = playback->getActiveVideoStream();
                for (const auto& v : variants) {
                    if (v.streamIndex != activeVideo) continue;
                    const size_t used = std::strlen(audioTrackBuf);
                    std::snprintf(audioTrackBuf + used, sizeof(audioTrackBuf) - used,
                                  used ? "  -  %dp%s" : "%dp%s", v.height,
                                  playback->isVideoQualityPinned() ? "" : " AUTO");
                    break;
                }
            }
        }

        p.audio_track = audioTrackBuf[0] ? audioTrackBuf : "";
        p.sub_track   = subTrackBuf[0]   ? subTrackBuf   : "";
        p.sub_delay_ms = prospero_subtitle_enabled ? prospero_subtitle_delay_ms : 0;
        p.is_live = playback->isLiveSource() ? 1 : 0;
        p.audio_badge = chLabel[0] ? chLabel : ((evo_audio_channels == 8) ? "7.1" : "STEREO");
        p.decoder_badge = (evo_pb_active_backend() == EVO_VDEC_BACKEND_NATIVE) ? "Hardware" : "Software";
        /* #103: what the upscaler did to the last frame, so it is visible on the
         * OSD rather than only in Media Info. Plain "Off" = switched off in
         * Settings -> no badge. */
        if (!playback->isMusicMode()) {
            static const struct { const char* label; const char* badge; int active; } k_up[] = {
                { "Sharp",                  "SHARP UPSCALE",        1 },
                { "AI (Standard)",          "AI UPSCALE",           1 },
                { "AI (Large)",             "AI UPSCALE · LARGE",   1 },
                { "AI (Maximum)",           "AI UPSCALE · MAX",     1 },
                { "Off (HDR source)",       "UPSCALE OFF · 10-BIT", 0 },
                { "Off (source >= output)", "UPSCALE OFF · NATIVE", 0 },
                { "Off (unavailable)",      "UPSCALE OFF",          0 },
            };
            const char* up = evo_agc_upscale_label();
            for (const auto& e : k_up) {
                if (std::strcmp(up, e.label) == 0) {
                    p.upscale_badge = e.badge;
                    p.upscale_active = e.active;
                    break;
                }
            }

            /* #105: what motion smoothing did to the last frame */
            static const struct { const char* label; const char* badge; int active; } k_sm[] = {
                { "Low",                    "SMOOTHING · LOW",      1 },
                { "High",                   "SMOOTHING · HIGH",     1 },
                { "Off (>= 50 fps)",        "SMOOTH OFF · HIGH FPS", 0 },
                { "Off (10-bit/HDR)",       "SMOOTH OFF · 10-BIT",  0 },
                { "Off (source > 1080p)",   "SMOOTH OFF · 4K",      0 },
                { "Off (unavailable)",      "SMOOTH OFF",           0 },
                { "Off (scratch alloc failed)", "SMOOTH OFF",       0 },
                { "Off (GPU over budget)",  "SMOOTH OFF · CAPPED",  0 },
            };
            const char* sm = evo_agc_motion_smoothing_label();
            for (const auto& e : k_sm) {
                if (std::strcmp(sm, e.label) == 0) {
                    p.smooth_badge = e.badge;
                    p.smooth_active = e.active;
                    break;
                }
            }
        }
        p.deepblack_badge = evo_agc_deepblack_badge();   /* #119 */
        p.position_sec = playback->getPositionSeconds();
        p.duration_sec = playback->getDurationSeconds();
        p.percentage = playback->getPercentage();
        p.paused = playback->isPaused() ? 1 : 0;
        p.scrub_active = playback->isScrubbing() ? 1 : 0;
        p.loading = (!playback->isMusicMode() &&
                     (!video_frame_loaded || g_pp_pb.seek_discarding)) ? 1 : 0;
        p.scrub_target = playback->getScrubTargetSeconds();
        p.view_mode = static_cast<int>(playback->getViewMode());
        p.show_stats = m_showStatsForNerds ? 1 : 0;
        p.alpha = m_osdVisibilityAlpha;
        p.subtitle_text = activeSubText[0] ? activeSubText : nullptr;
        p.subtitle_face = prospero_subtitle_face;
        p.subtitle_raised = 1;
        p.subtitle_text2 = secondarySubText[0] ? secondarySubText : nullptr;
        p.subtitle2_position = prospero_secondary_position;
        p.subtitle2_color = prospero_secondary_color;
        p.subtitle_colour = prospero_subtitle_colour;
        p.subtitle_background = prospero_subtitle_background;
        p.subtitle_position = prospero_subtitle_position_pref;
        p.chrome_hidden = 0;
        p.fps = perf_render_fps;
        p.debug_overlay = evo_fps_counter_enabled();
        p.music_mode = playback->isMusicMode() ? 1 : 0;
        p.music_codec = "AUDIO";

        evo_rmlui_update_playback_params(&p);
        if (m_showStatsForNerds) {
            feedPerformanceHud();
        }
        evo_rmlui_render_playback_osd(framebuffer, width, height);
    } else if ((activeSubText[0] || secondarySubText[0]) && !playback->isMusicMode() &&
               evo_rmlui_is_initialized()) {
        evo_playback_osd_params_t p;
        std::memset(&p, 0, sizeof(p));
        p.title = "Video Playback";
        p.subtitle_text = activeSubText;
        p.subtitle_face = prospero_subtitle_face;
        p.subtitle_raised = 0;
        p.subtitle_text2 = secondarySubText[0] ? secondarySubText : nullptr;
        p.subtitle2_position = prospero_secondary_position;
        p.subtitle2_color = prospero_secondary_color;
        p.subtitle_colour = prospero_subtitle_colour;
        p.subtitle_background = prospero_subtitle_background;
        p.subtitle_position = prospero_subtitle_position_pref;
        p.chrome_hidden = 1;
        p.alpha = 255;
        p.fps = perf_render_fps;
        p.debug_overlay = evo_fps_counter_enabled();

        evo_rmlui_update_playback_params(&p);
        evo_rmlui_render_playback_osd(framebuffer, width, height);
    }
}

} // namespace evo
