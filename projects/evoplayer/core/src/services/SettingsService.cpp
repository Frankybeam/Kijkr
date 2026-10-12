#include "evo/services/SettingsService.hpp"
#include "evo_data_path.h"
#include "evo_theme.h"
#include "evo_toast.h"
#include "evo_feedback.h"
#include "evo_keyboard.h"
#include "evo_rmlui_bridge.h"
#include "evo_subtitle.h"
#include "evo_vdec.h"

#include <cstdio>
#include <cstring>

namespace evo {

SettingsService::SettingsService() {
    m_themeName = "EVO Dark";
}

const char* SettingsService::getViewModeName(ViewMode mode) const {
    switch (mode) {
        case ViewMode::Fit:     return "FIT";
        case ViewMode::Fill:    return "FILL";
        case ViewMode::Stretch: return "STRETCH";
        default:                return "FIT";
    }
}

const char* SettingsService::getDecoderPreferenceBadge(DecoderPreference preference) const {
    switch (preference) {
        case DecoderPreference::Auto:           return "AUTO";
        case DecoderPreference::NativeHardware: return "HARDWARE (sceVideodec2)";
        case DecoderPreference::FFmpegSoftware: return "SOFTWARE (FFmpeg)";
        default:                                return "AUTO";
    }
}

const char* SettingsService::getUpscalerName(Upscaler upscaler) const {
    switch (upscaler) {
        case Upscaler::Sharp: return "SHARP";
        case Upscaler::AI:    return "AI";
        case Upscaler::Off:
        default:              return "OFF";
    }
}

const char* SettingsService::getAiNetworkName(AiNetwork network) const {
    switch (network) {
        case AiNetwork::Standard: return "STANDARD";
        case AiNetwork::Large:    return "LARGE";
        case AiNetwork::Maximum:  return "MAXIMUM (PRO)";
        case AiNetwork::Auto:
        default:                  return "AUTO";
    }
}

const char* SettingsService::getMotionSmoothingName(MotionSmoothing smoothing) const {
    switch (smoothing) {
        case MotionSmoothing::Low:  return "LOW";
        case MotionSmoothing::High: return "HIGH";
        case MotionSmoothing::Off:
        default:                    return "OFF";
    }
}

const char* SettingsService::getDeepBlacksName(DeepBlacks mode) const {
    switch (mode) {
        case DeepBlacks::Low:  return "LOW";
        case DeepBlacks::High: return "HIGH";
        case DeepBlacks::Off:
        default:               return "OFF";
    }
}

const char* SettingsService::getHdrOutputModeName(HdrOutputMode mode) const {
    switch (mode) {
        case HdrOutputMode::Off:  return "OFF (TONE-MAP TO SDR)";
        case HdrOutputMode::Auto:
        default:                  return "AUTO (MATCH CONTENT)";
    }
}

const char* SettingsService::getAudioOutputChannelsName(AudioOutputChannels channels) const {
    switch (channels) {
        case AudioOutputChannels::Stereo: return "STEREO (2.0)";
        case AudioOutputChannels::Auto:
        default:                          return "AUTO";
    }
}

const char* SettingsService::getRefreshRateModeName(RefreshRateMode mode) const {
    switch (mode) {
        case RefreshRateMode::Off:          return "OFF";
        case RefreshRateMode::Always:       return "AUTO (ALWAYS 120 HZ)";
        case RefreshRateMode::PlaybackOnly: return "PLAYBACK ONLY";
        default:                           return "OFF";
    }
}

void SettingsService::setThemeName(const std::string& themeName) {

    m_themeName = themeName;
    evo_theme_set_by_name(m_themeName.c_str());
    syncThemeToRmlUi();
    evo_feedback_refresh_lightbar();
}

void SettingsService::setSoundFeedbackEnabled(bool enabled) {
    m_soundFeedbackEnabled = enabled;
    evo_feedback_set_sound(enabled ? 1 : 0);
}

void SettingsService::setLightbarFeedbackEnabled(bool enabled) {
    m_lightbarFeedbackEnabled = enabled;
    evo_feedback_set_lightbar(enabled ? 1 : 0);
}

void SettingsService::setKeyboardType(int type) {
    m_keyboardType = type;
    evo_keyboard_set_type(type);
}

/*
 * The secondary caption's look is read by the player through the subtitle
 * engine's globals, next to prospero_subtitle_face, so a change is pushed
 * there as well as remembered here. Out-of-range values are refused rather than
 * clamped: they can only come from a hand-edited or newer settings file.
 */
void SettingsService::setSecondarySubtitlePosition(int position) {
    if (position < PROSPERO_SECONDARY_POS_STACKED || position > PROSPERO_SECONDARY_POS_TOP) return;
    m_secondarySubtitlePosition = position;
    prospero_secondary_position = position;
}

void SettingsService::setSecondarySubtitleColor(int color) {
    if (color < 0 || color >= PROSPERO_SECONDARY_COLORS) return;
    m_secondarySubtitleColor = color;
    prospero_secondary_color = color;
}

/*
 * The default subtitle size. The player has three sizes (prospero_subtitle_face
 * 1..3) and no other typefaces, so the old STANDARD / ROUNDED / BOLD /
 * CONDENSED choice had nothing behind it. 0..2 here; anything else from an
 * older settings file becomes MEDIUM.
 */
void SettingsService::setSubtitleFontFace(int face) {
    if (face < 0 || face > 2) face = 1;
    m_subtitleFontFace = face;
    prospero_subtitle_face = face + 1;
}

void SettingsService::setSubtitleColour(int colour) {
    if (colour < 0 || colour >= PROSPERO_SUBTITLE_COLOURS) return;
    m_subtitleColour = colour;
    prospero_subtitle_colour = colour;
}

void SettingsService::setSubtitleBackground(int background) {
    if (background < 0 || background >= PROSPERO_SUBTITLE_BACKGROUNDS) return;
    m_subtitleBackground = background;
    prospero_subtitle_background = background;
}

void SettingsService::setSubtitlePositionPref(int position) {
    if (position < 0 || position >= PROSPERO_SUBTITLE_POSITIONS) return;
    m_subtitlePositionPref = position;
    prospero_subtitle_position_pref = position;
}

void SettingsService::setAudioLanguagePref(int index) {
    if (index < 0 || index >= PROSPERO_LANG_COUNT) return;
    m_audioLangPref = index;
    prospero_audio_lang_pref = index;
}

void SettingsService::setSubtitleLanguagePref(int index) {
    if (index < 1 || index >= PROSPERO_LANG_COUNT) return;   /* subtitles always prefer one */
    m_subLangPref = index;
    prospero_subtitle_lang_pref = index;
}

void SettingsService::setAudioLanguagePref2(int index) {
    if (index < 0 || index >= PROSPERO_LANG_COUNT) return;   /* 0 = none */
    m_audioLangPref2 = index;
    prospero_audio_lang_pref2 = index;
}

void SettingsService::setSubtitleLanguagePref2(int index) {
    if (index < 0 || index >= PROSPERO_LANG_COUNT) return;   /* 0 = none */
    m_subLangPref2 = index;
    prospero_subtitle_lang_pref2 = index;
}

bool SettingsService::saveSettings() {
    const char* filePath = evo_data_path("evo_player_settings.cfg");
    FILE* file = std::fopen(filePath, "w");
    if (!file) {
        toast("SETTINGS", "SAVE FAILED");
        return false;
    }

    const char* activeTheme = evo_theme_name(evo_theme_index());
    if (!activeTheme || !activeTheme[0]) {
        activeTheme = m_themeName.c_str();
    }

    std::fprintf(file,
        "%d\n%d\n%d\n%d\n%d\n%d\n%s\n%d\n%d\n%d\n%d\n%d\n%d\n%d\n%d\n%d\n%d\n%d\n%d\n%d\n%d\n%d\n%d\n%d\n%d\n%d\n%d\n%d\n%d\n%d\n%d\n%d\n",
        0, // legacy dummy profile
        m_resumePlaybackEnabled ? 1 : 0,
        static_cast<int>(m_defaultViewMode),
        m_autoSubtitlesEnabled ? 1 : 0,
        m_debugOverlayEnabled ? 1 : 0,
        m_sortFoldersFirst ? 1 : 0,
        activeTheme,
        m_soundFeedbackEnabled ? 1 : 0,
        m_lightbarFeedbackEnabled ? 1 : 0,
        m_subtitleFontFace,
        m_keyboardType,
        static_cast<int>(m_decoderPreference),
        2, // keyboard preference saved with the native-IME default (see loadSettings)
        static_cast<int>(m_upscaler), // line 14 (#103)
        static_cast<int>(m_aiNetwork), // line 15 (#103)
        static_cast<int>(m_refreshRateMode), // line 16 (120 Hz output mode)
        static_cast<int>(m_hdrOutputMode),   // line 17 (HDR10 output)
        m_secondarySubtitlePosition,         // line 18 (#110)
        m_secondarySubtitleColor,            // line 19 (#110)
        m_askStream ? 1 : 0,                 // line 20 (live TV stream picker)
        static_cast<int>(m_audioOutputChannels), // line 21 (#117 stereo downmix)
        static_cast<int>(m_motionSmoothing), // line 22 (#105 motion smoothing)
        static_cast<int>(m_deepBlacks),      // line 23 (#119 deep blacks)
        m_audioPassthroughEnabled ? 1 : 0,   // line 24 (HDMI bitstream passthrough)
        m_matchVideoRate ? 1 : 0,            // line 25 (experimental: match video frame rate)
        m_subtitleColour,                    // line 26 (primary caption colour)
        m_subtitleBackground,                // line 27 (primary caption plate)
        m_subtitlePositionPref,              // line 28 (primary caption height)
        m_audioLangPref,                     // line 29 (preferred audio language)
        m_subLangPref,                       // line 30 (preferred subtitle language)
        m_subLangPref2,                      // line 31 (second subtitle language)
        m_audioLangPref2                     // line 32 (second audio language)
    );

    std::fclose(file);
    return true;
}

bool SettingsService::loadSettings() {
    const char* filePath = evo_data_path("evo_player_settings.cfg");
    FILE* file = std::fopen(filePath, "r");
    if (!file) {
        return false;
    }

    int rawProfile = 0;
    int rawResume = 1;
    int rawViewMode = 0;
    int rawAutoSubs = 1;
    int rawDebugOverlay = 0;
    int rawSortFolders = 1;
    char themeBuffer[128] = {0};
    int rawSoundFeedback = 1;
    int rawLightbarFeedback = 1;
    int rawSubtitleFace = 1;
    int rawKeyboardType = 0;
    int rawDecoderPref = EVO_VDEC_PREF_AUTO;
    int explicitKeyboardMarker = 0;
    int rawUpscaler = 0;
    int rawAiNetwork = 0;
    int rawRefreshRateMode = 0;
    int rawHdrOutputMode = 0;
    int rawSecondaryPosition = 0;
    int rawSecondaryColor = 0;
    int rawAskStream = 1;
    int rawAudioChannels = 0;
    int rawMotionSmoothing = 0;
    int rawDeepBlacks = 0;
    int rawAudioPassthrough = 0;
    int rawMatchVideoRate = 0;
    int rawSubColour = 0;
    int rawSubBackground = 1;
    int rawSubPosition = 0;
    int rawAudioLang = 0;
    int rawSubLang = 2;
    int rawSubLang2 = 2;
    int rawAudioLang2 = 0;

    int readCount = std::fscanf(file,
        "%d\n%d\n%d\n%d\n%d\n%d\n%127[^\n]\n%d\n%d\n%d\n%d\n%d\n%d\n%d\n%d\n%d\n%d\n%d\n%d\n%d\n%d\n%d\n%d\n%d\n%d\n%d\n%d\n%d\n%d\n%d\n%d\n%d",
        &rawProfile,
        &rawResume,
        &rawViewMode,
        &rawAutoSubs,
        &rawDebugOverlay,
        &rawSortFolders,
        themeBuffer,
        &rawSoundFeedback,
        &rawLightbarFeedback,
        &rawSubtitleFace,
        &rawKeyboardType,
        &rawDecoderPref,
        &explicitKeyboardMarker,
        &rawUpscaler,
        &rawAiNetwork,
        &rawRefreshRateMode,
        &rawHdrOutputMode,
        &rawSecondaryPosition,
        &rawSecondaryColor,
        &rawAskStream,
        &rawAudioChannels,
        &rawMotionSmoothing,
        &rawDeepBlacks,
        &rawAudioPassthrough,
        &rawMatchVideoRate,
        &rawSubColour,
        &rawSubBackground,
        &rawSubPosition,
        &rawAudioLang,
        &rawSubLang,
        &rawSubLang2,
        &rawAudioLang2
    );

    std::fclose(file);

    (void)rawProfile;
    if (readCount >= 2) m_resumePlaybackEnabled = (rawResume != 0);
    if (readCount >= 3) {
        if (rawViewMode >= 0 && rawViewMode <= 2) {
            m_defaultViewMode = static_cast<ViewMode>(rawViewMode);
        }
    }
    if (readCount >= 4) m_autoSubtitlesEnabled = (rawAutoSubs != 0);
    if (readCount >= 5) m_debugOverlayEnabled = (rawDebugOverlay != 0);
    if (readCount >= 6) m_sortFoldersFirst = (rawSortFolders != 0);
    if (readCount >= 7 && themeBuffer[0] != '\0') {
        m_themeName = themeBuffer;
        evo_theme_set_by_name(themeBuffer);
    }
    if (readCount >= 8) {
        m_soundFeedbackEnabled = (rawSoundFeedback != 0);
        evo_feedback_set_sound(rawSoundFeedback);
    }
    if (readCount >= 9) {
        m_lightbarFeedbackEnabled = (rawLightbarFeedback != 0);
        evo_feedback_set_lightbar(rawLightbarFeedback);
    }
    if (readCount >= 10) {
        setSubtitleFontFace(rawSubtitleFace);
    }
    if (readCount >= 11) {
        /* Marker 2 = saved with the corrected default. Files carrying
         * marker 1 were written by builds whose default was secretly the
         * virtual keyboard, so their value is not a real choice: they get
         * the native IME back, once, and are re-saved with marker 2. */
        if (readCount >= 13 && explicitKeyboardMarker == 2) {
            m_keyboardType = rawKeyboardType;
        } else {
            m_keyboardType = EVO_KEYBOARD_TYPE_NATIVE;
        }
        evo_keyboard_set_type(m_keyboardType);
    }
    if (readCount >= 12) {
        m_decoderPreference = static_cast<DecoderPreference>(rawDecoderPref);
    }
    if (readCount >= 14 && rawUpscaler >= 0 && rawUpscaler <= 2) {
        m_upscaler = static_cast<Upscaler>(rawUpscaler);
    }
    if (readCount >= 15 && rawAiNetwork >= 0 && rawAiNetwork <= 3) {
        m_aiNetwork = static_cast<AiNetwork>(rawAiNetwork);
    }
    if (readCount >= 16 && rawRefreshRateMode >= 0 && rawRefreshRateMode <= 2) {
        m_refreshRateMode = static_cast<RefreshRateMode>(rawRefreshRateMode);
    }
    if (readCount >= 17 && rawHdrOutputMode >= 0 && rawHdrOutputMode <= 1) {
        m_hdrOutputMode = static_cast<HdrOutputMode>(rawHdrOutputMode);
    }
    if (readCount >= 18) setSecondarySubtitlePosition(rawSecondaryPosition);
    if (readCount >= 19) setSecondarySubtitleColor(rawSecondaryColor);
    if (readCount >= 20) m_askStream = (rawAskStream != 0);
    if (readCount >= 21 && rawAudioChannels >= 0 && rawAudioChannels <= 1) {
        m_audioOutputChannels = static_cast<AudioOutputChannels>(rawAudioChannels);
    }
    if (readCount >= 22 && rawMotionSmoothing >= 0 && rawMotionSmoothing <= 2) {
        m_motionSmoothing = static_cast<MotionSmoothing>(rawMotionSmoothing);
    }
    if (readCount >= 23 && rawDeepBlacks >= 0 && rawDeepBlacks <= 2) {
        m_deepBlacks = static_cast<DeepBlacks>(rawDeepBlacks);
    }
    if (readCount >= 24) {
        m_audioPassthroughEnabled = (rawAudioPassthrough != 0);
    }
    if (readCount >= 25) m_matchVideoRate = (rawMatchVideoRate != 0);
    if (readCount >= 26) setSubtitleColour(rawSubColour);
    if (readCount >= 27) setSubtitleBackground(rawSubBackground);
    if (readCount >= 28) setSubtitlePositionPref(rawSubPosition);
    if (readCount >= 29) setAudioLanguagePref(rawAudioLang);
    if (readCount >= 30) setSubtitleLanguagePref(rawSubLang);
    if (readCount >= 31) setSubtitleLanguagePref2(rawSubLang2);
    if (readCount >= 32) setAudioLanguagePref2(rawAudioLang2);

    syncThemeToRmlUi();
    evo_feedback_refresh_lightbar();
    return true;
}

void SettingsService::syncThemeToRmlUi() {
    const evo_theme* t = evo_theme_current();
    if (!t) return;

    evo_rmlui_theme_t rmlUiTheme;
    rmlUiTheme.name = t->name;
    rmlUiTheme.bg_top = t->bg_top;
    rmlUiTheme.bg_bottom = t->bg_bottom;
    rmlUiTheme.surface = t->surface;
    rmlUiTheme.surface_sel = t->surface_sel;
    rmlUiTheme.border = t->border;
    rmlUiTheme.border_sel = t->border_sel;
    rmlUiTheme.accent = t->accent;
    rmlUiTheme.accent_soft = t->accent_soft;
    rmlUiTheme.accent_alt = t->accent_alt;
    rmlUiTheme.text_primary = t->text_primary;
    rmlUiTheme.text_secondary = t->text_secondary;
    rmlUiTheme.text_muted = t->text_muted;

    evo_rmlui_set_theme(&rmlUiTheme);
}

} // namespace evo
