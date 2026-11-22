// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "input/luna_client_messages.hpp"

#include <array>
#include <functional>
#include <string>
#include <vector>

namespace lunanx
{
inline constexpr std::array PlaybackHeights{720, 1080, 1440, 2160};
inline constexpr std::array PlaybackBitrates{5, 10, 15, 20, 25};
inline constexpr std::array PlaybackFrameRates{30, 60};

enum class PlaybackDecoder { Automatic, Hardware, Software };
// Spatial picture filter of LunaNX and NxNow: smooths low-contrast compression
// blocks and restores bounded edge detail. Standard and Strong use their
// Adaptive and Clarity strengths.
enum class PlaybackFilter { Off, Standard, Strong };
struct PictureFilterStrength
{
    float denoise;
    float sharpen;
};
PictureFilterStrength PictureFilter(PlaybackFilter filter);
// Bitrate mode. The stored values 2 and 3 were the former Clarity and
// Original priorities; they are read back as Automatic.
enum class PlaybackPriority { Automatic = 0, Stable = 1, Maximum = 4 };
inline constexpr std::array PlaybackPriorities{PlaybackPriority::Automatic, PlaybackPriority::Stable,
                                              PlaybackPriority::Maximum};
// Quality profile: the H.264 profile, the picture and the sound chosen
// together, from a weak connection (Low) to a wired one (Ultra). None leaves
// them to the automatic mode or to the custom rows.
enum class QualityProfile { None = 0, Low = 1, Medium = 2, High = 3, Ultra = 4 };
inline constexpr std::array QualityProfiles{QualityProfile::Low, QualityProfile::Medium, QualityProfile::High,
                                           QualityProfile::Ultra};
// The H.264 profile a session offers first; Constrained Baseline stays
// offered next to Main and High so the server can always fall back to it.
enum class H264Profile { ConstrainedBaseline, Main, High };
H264Profile OfferedH264Profile(QualityProfile profile, H264Profile platform_default);
// Stable never asks for more than this, whatever the maximum bitrate setting.
inline constexpr int StableBitrateCeilingMbps = 10;
const char* PlaybackPriorityLabel(PlaybackPriority priority);

struct PlaybackPreferences
{
    int height = 1080;
    int mbps = 20;
    PlaybackPriority priority = PlaybackPriority::Automatic;
    int fps = 60;
    int volume = 100;
    int buffer_ms = 40;
    bool audio = true;
    // Controller options, applied to every report sent to the game.
    bool swap_ab = false;
    bool swap_xy = false;
    bool invert_left_y = false;
    // On-screen keyboard shown and hidden with L1 + L2 + R1 + R2.
    bool virtual_keyboard = true;
    bool diagnostics = false;
    bool simple_library = false;
    std::string region = "Auto";
    std::string language;
    std::string market;
    PlaybackDecoder decoder = PlaybackDecoder::Automatic;
    PlaybackFilter filter = PlaybackFilter::Standard;
    // Resolution, frame rate, bitrate and mode follow the device instead of
    // the four values above. LoadPlaybackPreferences turns it on for a new
    // installation and for settings that were never changed.
    bool automatic = false;
    // Desktop players: catalog and games fill the screen without borders.
    bool fullscreen = false;
    // Low: Baseline, 720p 30 FPS, 5 Mbit/s, Stable, 60 ms of audio.
    // Medium: Main, 720p 60 FPS, 10 Mbit/s, Automatic.
    // High: High, 1080p 60 FPS, 20 Mbit/s, Luna's best quality.
    // Ultra: High, 1080p 60 FPS, 25 Mbit/s with a 15 Mbit/s floor, Luna's
    // best quality, full-rate Opus and 60 ms of audio. Meant for a wired
    // connection: over Wi-Fi the picture lags.
    QualityProfile profile = QualityProfile::None;
};

// The screen the player shows the stream on, in physical pixels. A player
// reports it before it starts a session; 0 means unknown.
struct DeviceDisplay
{
    int height = 0;
    int refresh_hz = 0;
};
void SetDeviceDisplay(DeviceDisplay display);
DeviceDisplay CurrentDeviceDisplay();
// Resolution, frame rate, bitrate and mode chosen by the automatic mode. Luna
// streams at most 1080p, so a larger screen gets 1080p and the interface,
// drawn at the screen's own resolution, stays separate from the stream.
PlaybackPreferences AutomaticVideo(PlaybackPreferences settings, DeviceDisplay display);
// The values a session uses: those of the profile, or the automatic ones when
// the mode is on. The profile itself is kept for the session.
PlaybackPreferences ResolvedPlaybackPreferences(const PlaybackPreferences& settings);

// Settings screens group the rows: general ones, the controller, and the
// technical rows under « Réglages avancés ».
enum class PlaybackSection { General, Controller, Advanced };

struct PlaybackChoice
{
    PlaybackSection section;
    std::string title;
    std::vector<std::string> labels;
    std::size_t selected;
    std::function<void(PlaybackPreferences&, std::size_t)> apply;
};

bool ValidPlaybackPreferences(const PlaybackPreferences& settings);
PlaybackPreferences LoadPlaybackPreferences();
bool SavePlaybackPreferences(const PlaybackPreferences& settings);
input::DisplayRequest PlaybackDisplay(const PlaybackPreferences& settings);
// The decoder, picture filter and full-screen rows are listed only by the
// players that honour them.
std::vector<PlaybackChoice> PlaybackChoices(const PlaybackPreferences& settings, bool decoder_choice = false,
                                            bool filter_choice = false, bool fullscreen_choice = false);
const char* PlaybackSectionTitle(PlaybackSection section);
}
