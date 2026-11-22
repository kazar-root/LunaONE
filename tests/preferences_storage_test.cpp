// SPDX-License-Identifier: GPL-3.0-only
// Settings written then read back, as every client does between two games.

#include "core/playback_preferences.hpp"

#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

int main()
{
    using namespace lunanx;
    const auto directory = std::filesystem::temp_directory_path() /
                           ("lunaone-preferences-" + std::to_string(std::rand()) + std::to_string(::time(nullptr)));
#if defined(_WIN32)
    _putenv_s("LUNAONE_DATA_DIR", directory.string().c_str());
#else
    setenv("LUNAONE_DATA_DIR", directory.c_str(), 1);
#endif

    // A new installation follows the device.
    assert(LoadPlaybackPreferences().automatic);

    PlaybackPreferences settings;
    settings.height = 720;
    settings.mbps = 10;
    settings.fps = 30;
    settings.swap_ab = true;
    settings.swap_xy = false;
    settings.invert_left_y = true;
    settings.virtual_keyboard = false;
    settings.fullscreen = true;
    assert(SavePlaybackPreferences(settings));
    auto loaded = LoadPlaybackPreferences();
    assert(loaded.height == 720 && loaded.mbps == 10 && loaded.fps == 30 && !loaded.automatic);
    assert(loaded.swap_ab && !loaded.swap_xy && loaded.invert_left_y && !loaded.virtual_keyboard && loaded.fullscreen);

    // Each controller row of the settings screen changes only its option.
    auto rows = PlaybackChoices(loaded);
    for (std::size_t i = 0; i < rows.size(); ++i)
        if (rows[i].title == "Swap X/Y" || rows[i].title == "Intervertir X/Y")
            rows[i].apply(loaded, 1);
    assert(SavePlaybackPreferences(loaded));
    loaded = LoadPlaybackPreferences();
    assert(loaded.swap_ab && loaded.swap_xy);

    // Ultra: saved, and it overrides the video values for a session.
    loaded.profile = QualityProfile::Ultra;
    assert(SavePlaybackPreferences(loaded));
    loaded = LoadPlaybackPreferences();
    const auto ultra = ResolvedPlaybackPreferences(loaded);
    assert(loaded.profile == QualityProfile::Ultra && ultra.height == 1080 && ultra.fps == 60 && ultra.mbps == 25 &&
           ultra.priority == PlaybackPriority::Maximum && ultra.buffer_ms >= 60 &&
           ultra.profile == QualityProfile::Ultra);
    // The other profiles, and their H.264 profile.
    loaded.profile = QualityProfile::Low;
    assert(SavePlaybackPreferences(loaded) && LoadPlaybackPreferences().profile == QualityProfile::Low);
    const auto low = ResolvedPlaybackPreferences(loaded);
    assert(low.height == 720 && low.fps == 30 && low.mbps == 5 && low.priority == PlaybackPriority::Stable &&
           low.buffer_ms >= 60);
    loaded.profile = QualityProfile::Medium;
    assert(ResolvedPlaybackPreferences(loaded).height == 720 && ResolvedPlaybackPreferences(loaded).fps == 60);
    assert(OfferedH264Profile(QualityProfile::Low, H264Profile::High) == H264Profile::ConstrainedBaseline &&
           OfferedH264Profile(QualityProfile::Medium, H264Profile::ConstrainedBaseline) == H264Profile::Main &&
           OfferedH264Profile(QualityProfile::High, H264Profile::ConstrainedBaseline) == H264Profile::High &&
           OfferedH264Profile(QualityProfile::None, H264Profile::High) == H264Profile::High);
    // A file written before V0.1 (here with the former Ultra switch) starts
    // again on the automatic quality, its other choices kept.
    {
        std::ofstream file(directory / "playback_preferences.txt");
        file << "1080 20 0\n60 100 100 1 1 0 0 Auto - - 0 1 1 0 0 0 1 1 4\n";
    }
    const auto reset = LoadPlaybackPreferences();
    assert(reset.automatic && reset.profile == QualityProfile::None && reset.buffer_ms == 40 && reset.swap_ab &&
           reset.swap_xy);
    loaded.profile = QualityProfile::Ultra;
    assert(PlaybackDisplay(loaded).max_bitrate == 25'000'000 && PlaybackDisplay(loaded).start_bitrate == 25'000'000 &&
           PlaybackDisplay(loaded).min_bitrate == 15'000'000);

    // A file of an earlier version: one value swapped A/B and X/Y together.
    {
        std::ofstream file(directory / "playback_preferences.txt");
        file << "720 10 1\n30 100 40 1 1 0 0 Auto - - 0 1\n";
    }
    loaded = LoadPlaybackPreferences();
    assert(loaded.swap_ab && loaded.swap_xy && !loaded.invert_left_y && loaded.virtual_keyboard);
    std::filesystem::remove_all(directory);
}
