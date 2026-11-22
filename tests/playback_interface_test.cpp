// SPDX-License-Identifier: GPL-3.0-only

#include "core/i18n.hpp"
#include "core/playback_preferences.hpp"
#include "input/controller_labels.hpp"
#include "stream/performance_overlay.hpp"
#include "stream/audio/AudioReorderBuffer.hpp"
#include "stream/audio/PcmBuffer.hpp"
#include "stream/DecodeQueuePolicy.hpp"
#include "stream/launch_status.hpp"
#include "ui/device_qr.hpp"
#include "ui/library_sections.hpp"

#include <algorithm>
#include <cassert>
#include <cstdint>

int main()
{
    using namespace lunanx;
    for (const auto height : PlaybackHeights)
        for (const auto rate : PlaybackBitrates)
            for (const auto fps : PlaybackFrameRates)
                for (const auto priority : PlaybackPriorities)
                {
                    const auto request = PlaybackDisplay({height, rate, priority, fps});
                    assert(request.height == height && request.width == height * 16 / 9);
                    const auto ceiling = priority == PlaybackPriority::Stable
                        ? std::min(rate, StableBitrateCeilingMbps) : rate;
                    assert(request.fps == fps && request.max_bitrate == ceiling * 1'000'000u);
                    assert(request.min_bitrate <= request.start_bitrate && request.start_bitrate <= request.max_bitrate);
                }
    assert(!ValidPlaybackPreferences({1080, 42}));

    // Picture filter: the LunaNX and NxNow strengths, listed only by the
    // players that apply it, Standard by default.
    assert(PictureFilter(PlaybackFilter::Off).denoise == 0.0f && PictureFilter(PlaybackFilter::Off).sharpen == 0.0f);
    assert(PictureFilter(PlaybackFilter::Standard).denoise == 0.10f &&
           PictureFilter(PlaybackFilter::Standard).sharpen == 0.14f);
    assert(PictureFilter(PlaybackFilter::Strong).denoise == 0.13f && PictureFilter(PlaybackFilter::Strong).sharpen == 0.20f);
    const auto listed = PlaybackChoices(PlaybackPreferences{}, true, true);
    assert(listed.size() == PlaybackChoices(PlaybackPreferences{}).size() + 2);
    const auto filter_row = std::ranges::find_if(listed, [](const auto& row) { return row.title == Tr("Filtre d’image"); });
    assert(filter_row != listed.end() && filter_row->labels.size() == 3 && filter_row->selected == 1);
    PlaybackPreferences strong;
    filter_row->apply(strong, 2);
    assert(strong.filter == PlaybackFilter::Strong && ValidPlaybackPreferences(strong));
    assert(!ValidPlaybackPreferences({.filter = static_cast<PlaybackFilter>(7)}));
    assert(PlaybackDisplay({1080, 42}).height == 1080);
    assert(PlaybackDisplay({1080, 20, PlaybackPriority::Maximum, 30}).fps == 30);
    const auto maximum = PlaybackDisplay({1080, 20, PlaybackPriority::Maximum, 60});
    assert(maximum.fps == 60 && maximum.start_bitrate == maximum.max_bitrate);
    // A raised floor made the sender queue pictures: it stays low.
    assert(maximum.min_bitrate == 4'000'000);
    assert(PlaybackDisplay({1080, 20, PlaybackPriority::Automatic, 60}).min_bitrate == 4'000'000);
    const auto encoded = input::EncodeDisplayRequest(PlaybackDisplay({1080, 20}));
    const std::string message(encoded.begin(), encoded.end());
    assert(message.find("dynamic-downsampling") == std::string::npos);
    auto downsampling = PlaybackDisplay({1080, 20});
    downsampling.dynamic_downsampling = true;
    const auto announced = input::EncodeDisplayRequest(downsampling);
    const std::string with_profile(announced.begin(), announced.end());
    // Field 5 (wire type 2) carries the profile name, as LunaNX sends it.
    assert(with_profile.find(std::string("\x2a\x14") + "dynamic-downsampling") != std::string::npos);
    assert(with_profile.size() == message.size() + 22);
    // L3 + R3 + R1 leaves the game; two of the three or the former chord do not.
    constexpr auto sticks = static_cast<std::uint16_t>(input::kLeftThumb | input::kRightThumb);
    assert(!input::ExitStreamChord({input::kStart | input::kRightShoulder, 0, 255}));
    assert(!input::ExitStreamChord({sticks}));
    assert(!input::ExitStreamChord({static_cast<std::uint16_t>(input::kLeftThumb | input::kRightShoulder)}));
    assert(input::ExitStreamChord({static_cast<std::uint16_t>(sticks | input::kRightShoulder)}));
    assert(input::ExitStreamChord({static_cast<std::uint16_t>(sticks | input::kRightShoulder | input::kA)}));

    // Controller options: each swap is separate, the left stick Y axis flips.
    input::GamepadState face{static_cast<std::uint16_t>(input::kA | input::kX | input::kStart)};
    face.left_y = 1200;
    face.right_y = 900;
    const auto ab = input::ApplyControllerOptions(face, {true, false, false});
    assert(ab.buttons == (input::kB | input::kX | input::kStart) && ab.left_y == 1200);
    const auto xy = input::ApplyControllerOptions(face, {false, true, false});
    assert(xy.buttons == (input::kA | input::kY | input::kStart));
    const auto inverted = input::ApplyControllerOptions(face, {true, true, true});
    assert(inverted.buttons == (input::kB | input::kY | input::kStart) && inverted.left_y == -1200 &&
           inverted.right_y == 900);
    face.left_y = -32768;
    assert(input::ApplyControllerOptions(face, {false, false, true}).left_y == 32767);
    assert(input::ApplyControllerOptions(face, {}) == face);

    // Automatic mode: the device screen picks the stream, capped at 1080p.
    PlaybackPreferences automatic_mode;
    automatic_mode.automatic = true;
    automatic_mode.height = 720;
    automatic_mode.mbps = 5;
    const auto unknown = PlaybackDisplay(automatic_mode);
    assert(unknown.height == 1080 && unknown.fps == 60 && unknown.max_bitrate == 25'000'000 &&
           unknown.start_bitrate == unknown.max_bitrate);
    SetDeviceDisplay({1964, 120});
    assert(PlaybackDisplay(automatic_mode).height == 1080 && PlaybackDisplay(automatic_mode).fps == 60);
    SetDeviceDisplay({768, 60});
    assert(PlaybackDisplay(automatic_mode).height == 720);
    SetDeviceDisplay({2160, 30});
    assert(PlaybackDisplay(automatic_mode).height == 1080 && PlaybackDisplay(automatic_mode).fps == 30);
    // A custom choice keeps its own values whatever the screen.
    automatic_mode.automatic = false;
    assert(PlaybackDisplay(automatic_mode).height == 720 && PlaybackDisplay(automatic_mode).max_bitrate == 5'000'000);
    SetDeviceDisplay({});

    // Settings rows: changing a video row leaves the automatic mode and keeps
    // the values it was using; « Automatique » brings it back.
    PlaybackPreferences rows_settings;
    rows_settings.automatic = true;
    const auto rows = PlaybackChoices(rows_settings, true, true, true);
    const auto row = [&](const char* title) {
        const auto found = std::ranges::find_if(rows, [&](const auto& choice) { return choice.title == Tr(title); });
        assert(found != rows.end());
        return *found;
    };
    assert(row("Intervertir A/B").section == PlaybackSection::Controller);
    assert(row("Résolution").section == PlaybackSection::Advanced && row("Résolution").labels[row("Résolution").selected] == "1080p");
    assert(row("Profil de qualité").selected == 0 && row("Affichage").section == PlaybackSection::General &&
           row("Profil de qualité").section == PlaybackSection::General && row("Profil de qualité").labels.size() == 6);
    auto changed = rows_settings;
    row("Images par seconde").apply(changed, 0);
    assert(!changed.automatic && changed.fps == 30 && changed.height == 1080 && changed.mbps == 25 &&
           changed.priority == PlaybackPriority::Maximum);
    row("Profil de qualité").apply(changed, 0);
    assert(changed.automatic);
    // A profile, then a technical row: back to custom values.
    row("Profil de qualité").apply(changed, 2);
    assert(changed.profile == QualityProfile::Medium && !changed.automatic);
    row("Résolution").apply(changed, 1);
    assert(changed.profile == QualityProfile::None && changed.height == 1080 && changed.fps == 60 && changed.mbps == 10);
    row("Profil de qualité").apply(changed, 0);
    assert(changed.automatic && changed.profile == QualityProfile::None);
    row("Inverser l’axe Y du joystick gauche").apply(changed, 1);
    assert(changed.invert_left_y && ValidPlaybackPreferences(changed));
    assert(row("Clavier virtuel").section == PlaybackSection::Controller &&
           row("Clavier virtuel").selected == 1);
    row("Clavier virtuel").apply(changed, 0);
    assert(!changed.virtual_keyboard);
    assert(PlaybackChoices(rows_settings).size() + 3 == rows.size());

    // Button names follow the controller in hand.
    using input::ControllerFamily;
    assert(input::ControllerFamilyFromName("DualSense Wireless Controller") == ControllerFamily::PlayStation);
    assert(input::ControllerFamilyFromName("Pro Controller") == ControllerFamily::Nintendo);
    assert(input::ControllerFamilyFromName("Xbox Wireless Controller") == ControllerFamily::Xbox);
    assert(input::ControllerFamilyFromVendor(0x054c) == ControllerFamily::PlayStation);
    assert(input::ExitChordName(ControllerFamily::Xbox) == "LS + RS + RB");
    assert(input::ExitChordName(ControllerFamily::PlayStation) == "L3 + R3 + R1");
    assert(input::KeyboardChordName(ControllerFamily::Xbox) == "LB + LT + RB + RT");
    assert(input::KeyboardChordName(ControllerFamily::PlayStation) == "L1 + L2 + R1 + R2");
    assert(input::KeyboardChordName(ControllerFamily::Nintendo) == "L + ZL + R + ZR");
    assert(input::PadButtonName(ControllerFamily::PlayStation, input::PadButton::A) == "✕");
    assert(Tr("Infos techniques en jeu") != "" && TrFormat("Quitter une partie : {}", {"LS + RS + RB"}).find("RB") != std::string::npos);
    assert(LaunchStage("Preparing WebRTC and measuring Luna regions") == 1);
    assert(LaunchStage("Requesting a Luna game session") == 2);
    assert(LaunchStage("WebRTC completed") == 3);
    assert(LaunchPulseIndex(std::chrono::milliseconds(0)) == 0);
    assert(LaunchPulseIndex(std::chrono::milliseconds(200)) == 1);
    assert(LaunchPulseIndex(std::chrono::milliseconds(400)) == 2);
    assert(LaunchPulseIndex(std::chrono::milliseconds(600)) == 0);

    using namespace std::chrono_literals;
    const auto start = audio::AudioReorderBuffer::Clock::time_point{};
    audio::AudioReorderBuffer jitter(20ms);
    const std::array<std::uint8_t, 1> packet{42};
    std::vector<std::uint16_t> heard;
    const auto deliver = [&](auto bytes, auto sequence) {
        assert(bytes[0] == 42);
        heard.push_back(sequence);
    };
    jitter.Submit(packet, 65534, start, deliver);
    jitter.Submit(packet, 0, start + 5ms, deliver);
    jitter.Tick(start + 22ms, deliver);
    assert(heard.size() == 1);
    jitter.Submit(packet, 65535, start + 23ms, deliver);
    assert((heard == std::vector<std::uint16_t>{65534, 65535, 0}));
    jitter.Submit(packet, 2, start + 30ms, deliver);
    jitter.Tick(start + 50ms, deliver);
    jitter.Submit(packet, 1, start + 51ms, deliver);
    assert((heard == std::vector<std::uint16_t>{65534, 65535, 0, 2}));

    assert(video::MaximumBurstAccessUnits(60) >= 10);
    assert(video::MaximumBurstAccessUnits(30) >= 6);
    const std::array<std::uint8_t, 6> idr{0, 0, 0, 1, 0x65, 0xff};
    assert(video::IsH264Keyframe(idr));

    jitter.Submit(packet, 4, start + 60ms, deliver);
    jitter.Submit(packet, 6, start + 61ms, deliver);
    jitter.Tick(start + 81ms, deliver);
    assert((heard == std::vector<std::uint16_t>{65534, 65535, 0, 2, 4, 6}));

    jitter.Submit(packet, 8, start + 100ms, deliver, packet);
    jitter.Tick(start + 120ms, deliver);
    assert(heard[heard.size() - 2] == 7 && heard.back() == 8 && jitter.RedRepairs() == 1);
    jitter.Submit(packet, 9, start + 130ms, deliver, packet);
    assert(heard.back() == 9 && jitter.RedRepairs() == 1);

    audio::PcmBuffer pcm(30);
    std::array<std::int16_t, 960> block;
    block.fill(99);
    std::array<std::int16_t, 960> output{};
    pcm.Submit(block);
    assert(pcm.Render(output) == 0 && output.front() == 0);
    pcm.Submit(block);
    pcm.Submit(block);
    for (int i = 0; i < 3; ++i)
        assert(pcm.Render(output) == output.size() && output.front() == 99);
    assert(pcm.Render(output) == 0 && pcm.Underruns() == 1);
    pcm.Submit(block);
    assert(pcm.Render(output) == 0 && pcm.Underruns() == 1);
    // After an underrun, half of the 30 ms target is enough to resume.
    pcm.Submit(block);
    assert(pcm.Render(output) == output.size() && output.front() == 99);

    for (int i = 0; i < 3; ++i) pcm.Render(output);
    assert(pcm.Underruns() == 2 && pcm.TargetMs() == 50);
    for (int i = 0; i < 20; ++i) pcm.Submit(block);
    // A burst is kept: playback needs it to get through the next stall.
    assert(pcm.Render(output) == output.size() && pcm.TrimmedFrames() == 0);
    // Latency that stays above the target for a whole second is trimmed.
    for (int i = 0; i < 100; ++i)
    {
        pcm.Submit(block);
        assert(pcm.Render(output) == output.size());
    }
    assert(pcm.TrimmedFrames() > 0);
    // A stall right after a burst is then bridged without an underrun.
    pcm.Submit(block);
    for (int i = 0; i < 20; ++i) pcm.Submit(block);
    const auto underruns = pcm.Underruns();
    for (int i = 0; i < 20; ++i)
        assert(pcm.Render(output) == output.size());
    assert(pcm.Underruns() == underruns);

    const auto stable = PlaybackDisplay({1080, 25, PlaybackPriority::Stable});
    assert(stable.start_bitrate == 6'500'000 && stable.max_bitrate == 10'000'000);
    const auto automatic = PlaybackDisplay({720, 25, PlaybackPriority::Automatic, 30});
    assert(automatic.start_bitrate == 15'000'000 && automatic.max_bitrate == 25'000'000);

    SetUiLanguageForMarket("");
    assert(UiLanguage() == "en" && Tr("Créer un code") == "Create a code");
    SetUiLanguageForMarket("pl");
    assert(UiLanguage() == "pl" && Tr("Créer un code") == "Utwórz kod");
    SetUiLanguageForMarket("fr");
    assert(UiLanguage() == "fr" && Tr("Créer un code") == "Créer un code");

    const auto qr = ui::MakeDeviceQr("https://www.amazon.fr/code");
    assert(qr.size > 28 && qr.dark.size() == static_cast<std::size_t>(qr.size * qr.size));
    assert(qr.dark[4 * qr.size + 4]);
    for (int y = 0; y < qr.size; ++y)
        for (int x = 0; x < qr.size; ++x)
            if (x < 4 || y < 4 || x >= qr.size - 4 || y >= qr.size - 4)
                assert(qr.dark[y * qr.size + x] == 0);
    assert(ui::MakeDeviceQr("http://insecure").size == 0);

    PerformanceOverlay overlay;
    input::GamepadState pad{};
    pad.buttons = input::kLeftThumb;
    assert(overlay.Update(pad).buttons == input::kLeftThumb);
    pad.buttons |= input::kRightThumb;
    assert(overlay.Update(pad).buttons == 0 && overlay.Visible());
    assert(overlay.Update(pad).buttons == 0 && overlay.Visible());
    pad.buttons = input::kRightThumb;
    assert(overlay.Update(pad).buttons == 0 && overlay.Visible());
    overlay.Update({});
    pad.buttons |= input::kLeftThumb;
    assert(overlay.Update(pad).buttons == 0 && !overlay.Visible());

    luna::Game hero; hero.title = "Featured"; hero.asin = "1"; hero.image_landscape = "cover";
    luna::Game first; first.title = "Ranking"; first.asin = "2";
    std::vector<luna::Shelf> shelves{{"Suggestions", "FEATURED_ROW", {hero}}, {"Top", "RANKINGS_ROW", {first, first}}};
    const auto top = ui::LibraryGames(ui::LibrarySection::Top, shelves, {first, hero});
    assert(top.size() == 1 && top.front().asin == first.asin);
    assert(ui::LibraryGames(ui::LibrarySection::All, shelves, {first, hero}).front().asin == hero.asin);
    auto premium = first;
    premium.premium_only = true;
    // All the games: the included ones first, the locked ones after.
    const auto included = ui::LibraryGames(ui::LibrarySection::All, shelves, {premium, hero}, luna::AccountPlan::Standard);
    assert(included.size() == 2 && included.front().asin == hero.asin && included.back().premium_only);
    const auto upgrades = ui::LibraryGames(ui::LibrarySection::Premium, shelves, {premium, hero}, luna::AccountPlan::Premium);
    assert(upgrades.size() == 1 && upgrades.front().asin == first.asin);
}
