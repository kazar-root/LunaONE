// SPDX-License-Identifier: GPL-3.0-only

#include "playback_preferences.hpp"
#include "app_paths.hpp"
#include "atomic_file_replace.hpp"
#include "i18n.hpp"
#include "luna/amazon_auth.hpp"
#include "stream/video_quality_policy.hpp"

#include <algorithm>
#include <atomic>
#include <fstream>
#include <mutex>
#include <sstream>

namespace lunanx
{
namespace
{
std::mutex storage_lock;
constexpr std::array Decoders{PlaybackDecoder::Automatic, PlaybackDecoder::Hardware, PlaybackDecoder::Software};
constexpr std::array Filters{PlaybackFilter::Off, PlaybackFilter::Standard, PlaybackFilter::Strong};
constexpr std::array Buffers{30, 40, 60, 100};
constexpr std::array Volumes{33, 67, 100, 133};
constexpr std::array Regions{"Auto", "eu-west-3", "eu-west-2", "eu-central-1", "eu-central-2", "eu-south-1",
    "eu-south-2", "eu-north-1", "us-east-1", "us-east-2", "us-west-1", "us-west-2"};

std::string Path() { return AppHomePath() + "/playback_preferences.txt"; }

// Written last on line 2. Files without it predate V0.1, where every client
// starts on the automatic quality: Luna adapts the stream to what it measures,
// and the user may change it afterwards in the settings.
constexpr int kQualityDefaults = 1;

template<class Range, class T>
bool Contains(const Range& values, const T& value)
{
    return std::ranges::find(values, value) != std::ranges::end(values);
}
}

const char* PlaybackPriorityLabel(PlaybackPriority priority)
{
    switch (priority)
    {
    case PlaybackPriority::Stable: return "Stable";
    case PlaybackPriority::Maximum: return "Qualité maximale";
    default: return "Automatique";
    }
}

PictureFilterStrength PictureFilter(PlaybackFilter filter)
{
    if (filter == PlaybackFilter::Off)
        return {0.0f, 0.0f};
    const auto tuning = video::ResolveQualityTuning(filter == PlaybackFilter::Strong ? "Clarity" : "Adaptive");
    return {tuning.denoise_strength, tuning.sharpen_strength};
}

H264Profile OfferedH264Profile(QualityProfile profile, H264Profile platform_default)
{
    switch (profile)
    {
    case QualityProfile::Low: return H264Profile::ConstrainedBaseline;
    case QualityProfile::Medium: return H264Profile::Main;
    case QualityProfile::High:
    case QualityProfile::Ultra: return H264Profile::High;
    default: return platform_default;
    }
}

bool ValidPlaybackPreferences(const PlaybackPreferences& settings)
{
    const auto& markets = luna::AmazonMarkets();
    return Contains(PlaybackHeights, settings.height) && Contains(PlaybackBitrates, settings.mbps) &&
           Contains(PlaybackPriorities, settings.priority) && Contains(PlaybackFrameRates, settings.fps) &&
           Contains(Buffers, settings.buffer_ms) && Contains(Volumes, settings.volume) &&
           Contains(Regions, settings.region) && Contains(Decoders, settings.decoder) &&
           Contains(Filters, settings.filter) &&
           (settings.profile == QualityProfile::None || Contains(QualityProfiles, settings.profile)) &&
           (settings.language.empty() || IsSupportedUiLanguage(settings.language)) &&
           (settings.market.empty() || std::ranges::any_of(markets, [&](const auto& market) {
               return market.code == settings.market;
           }));
}

namespace
{
std::atomic<int> device_height {0};
std::atomic<int> device_refresh {0};

// Settings saved before the automatic mode that still hold the defaults.
bool UntouchedVideo(const PlaybackPreferences& settings)
{
    const PlaybackPreferences defaults;
    return settings.height == defaults.height && settings.mbps == defaults.mbps &&
           settings.priority == defaults.priority && settings.fps == defaults.fps;
}
}

void SetDeviceDisplay(DeviceDisplay display)
{
    device_height.store(std::max(0, display.height));
    device_refresh.store(std::max(0, display.refresh_hz));
}

DeviceDisplay CurrentDeviceDisplay() { return {device_height.load(), device_refresh.load()}; }

PlaybackPreferences AutomaticVideo(PlaybackPreferences settings, DeviceDisplay display)
{
    // The configuration measured on the Mac: 1080p, 60 FPS, a 25 Mbit/s
    // ceiling and the Maximum mode, whose floor stays low.
    settings.height = display.height > 0 && display.height < 1000 ? 720 : 1080;
    settings.fps = display.refresh_hz > 0 && display.refresh_hz < 50 ? 30 : 60;
    settings.mbps = 25;
    settings.priority = PlaybackPriority::Maximum;
    return settings;
}

PlaybackPreferences ResolvedPlaybackPreferences(const PlaybackPreferences& settings)
{
    if (settings.profile != QualityProfile::None)
    {
        struct Preset
        {
            int height, fps, mbps;
            PlaybackPriority priority;
            int min_buffer_ms;
        };
        const auto preset = settings.profile == QualityProfile::Low ? Preset{720, 30, 5, PlaybackPriority::Stable, 60} :
            settings.profile == QualityProfile::Medium ? Preset{720, 60, 10, PlaybackPriority::Automatic, 40} :
            settings.profile == QualityProfile::High ? Preset{1080, 60, 20, PlaybackPriority::Maximum, 40} :
                                                        Preset{1080, 60, 25, PlaybackPriority::Maximum, 60};
        auto resolved = settings;
        resolved.height = preset.height;
        resolved.fps = preset.fps;
        resolved.mbps = preset.mbps;
        resolved.priority = preset.priority;
        resolved.buffer_ms = std::max(resolved.buffer_ms, preset.min_buffer_ms);
        resolved.automatic = false;
        return resolved;
    }
    if (!settings.automatic) return settings;
    auto resolved = AutomaticVideo(settings, CurrentDeviceDisplay());
    resolved.automatic = false;
    return resolved;
}

namespace
{
// Video, frame rate and audio buffer back to « Automatique ».
PlaybackPreferences AutomaticQuality(PlaybackPreferences settings)
{
    const PlaybackPreferences defaults;
    settings.automatic = true;
    settings.profile = QualityProfile::None;
    settings.buffer_ms = defaults.buffer_ms;
    return settings;
}
}

PlaybackPreferences LoadPlaybackPreferences()
{
    std::lock_guard lock(storage_lock);
    std::ifstream file(Path());
    std::string line;
    std::getline(file, line);
    std::istringstream first(line);
    PlaybackPreferences settings;
    settings.automatic = true;
    if (!(first >> settings.height >> settings.mbps) || !ValidPlaybackPreferences(settings)) return settings;
    settings.automatic = false;
    int priority = 0;
    if (first >> priority)
    {
        // 2 and 3 were Clarity and Original, which became Automatic.
        if (priority == 2 || priority == 3) priority = static_cast<int>(PlaybackPriority::Automatic);
        if (Contains(PlaybackPriorities, static_cast<PlaybackPriority>(priority)))
            settings.priority = static_cast<PlaybackPriority>(priority);
    }
    settings.automatic = UntouchedVideo(settings);

    auto extended = settings;
    std::string language, market;
    // The fifth value was a single A/B + X/Y swap: it now swaps A/B, and X/Y
    // too when the file predates the separate X/Y row.
    if (file >> extended.fps >> extended.volume >> extended.buffer_ms >> extended.audio >> extended.swap_ab
             >> extended.diagnostics >> extended.simple_library >> extended.region >> language >> market)
    {
        if (language != "-") extended.language = language;
        if (market != "-") extended.market = market;
        extended.swap_xy = extended.swap_ab;
        extended.automatic = UntouchedVideo(extended);
        int decoder = 0;
        if (file >> decoder && Contains(Decoders, static_cast<PlaybackDecoder>(decoder)))
        {
            extended.decoder = static_cast<PlaybackDecoder>(decoder);
            // Files written before the picture filter keep its default.
            int filter = 0;
            if (file >> filter && Contains(Filters, static_cast<PlaybackFilter>(filter)))
            {
                extended.filter = static_cast<PlaybackFilter>(filter);
                // Then the controller rows, the automatic mode and full screen.
                bool swap_xy = false, invert = false, automatic = false, fullscreen = false;
                if (file >> swap_xy >> invert >> automatic >> fullscreen)
                {
                    extended.swap_xy = swap_xy;
                    extended.invert_left_y = invert;
                    extended.automatic = automatic;
                    extended.fullscreen = fullscreen;
                    bool keyboard = true, ultra = false;
                    if (file >> keyboard) extended.virtual_keyboard = keyboard;
                    // The former Ultra switch, then the quality profile.
                    if (file >> ultra && ultra) extended.profile = QualityProfile::Ultra;
                    int profile = 0, defaults = 0;
                    if (file >> profile && (profile == 0 || Contains(QualityProfiles, static_cast<QualityProfile>(profile))))
                        extended.profile = static_cast<QualityProfile>(profile);
                    if (file >> defaults && defaults >= kQualityDefaults) return ValidPlaybackPreferences(extended) ? extended : settings;
                }
            }
        }
        if (ValidPlaybackPreferences(extended)) return AutomaticQuality(extended);
    }
    return AutomaticQuality(settings);
}

bool SavePlaybackPreferences(const PlaybackPreferences& settings)
{
    if (!ValidPlaybackPreferences(settings)) return false;
    {
        std::lock_guard lock(storage_lock);
        PrepareAppStorage();
        std::ostringstream file;
        file << settings.height << ' ' << settings.mbps << ' ' << static_cast<int>(settings.priority) << '\n'
             << settings.fps << ' ' << settings.volume << ' ' << settings.buffer_ms << ' ' << settings.audio << ' '
             << settings.swap_ab << ' ' << settings.diagnostics << ' ' << settings.simple_library << ' '
             << settings.region << ' ' << (settings.language.empty() ? "-" : settings.language) << ' '
             << (settings.market.empty() ? "-" : settings.market) << ' ' << static_cast<int>(settings.decoder) << ' '
             << static_cast<int>(settings.filter) << ' ' << settings.swap_xy << ' ' << settings.invert_left_y << ' '
             << settings.automatic << ' ' << settings.fullscreen << ' ' << settings.virtual_keyboard << ' '
             << (settings.profile == QualityProfile::Ultra) << ' ' << static_cast<int>(settings.profile) << ' '
             << kQualityDefaults << '\n';
        if (!WriteFileAtomically(Path(), file.str())) return false;
    }
    if (!settings.language.empty()) SetUiLanguage(settings.language);
    return true;
}

input::DisplayRequest PlaybackDisplay(const PlaybackPreferences& requested)
{
    const auto settings = ResolvedPlaybackPreferences(ValidPlaybackPreferences(requested) ? requested : PlaybackPreferences{});
    const auto priority = settings.priority;
    const int mbps = priority == PlaybackPriority::Stable ? std::min(settings.mbps, StableBitrateCeilingMbps)
                                                          : settings.mbps;
    const auto bitrate = static_cast<std::uint32_t>(mbps * 1'000'000);
    // Maximum starts at the ceiling. Automatic starts at 60 % and climbs to
    // it within seconds; Stable starts at 65 % of its lower ceiling.
    const std::uint32_t start = priority == PlaybackPriority::Maximum ? bitrate :
        bitrate / 100 * (priority == PlaybackPriority::Stable ? 65 : 60);
    // The floor stays low in the other profiles. The server paces packets out
    // at its own estimate; a higher floor lets fast scenes encode above it,
    // and over Wi-Fi those pictures queued for one to three seconds at the
    // sender before they left. Ultra keeps the 15 Mbit/s floor of the Mac run
    // of 2026-11-20 at 21:47: Luna's encoder then streamed 10 to 14 Mbit/s
    // instead of about 6, for a sharp picture on a wired connection.
    const std::uint32_t floor = settings.profile == QualityProfile::Ultra ? 15'000'000u
                                                                          : std::min(4'000'000u, bitrate / 2);
    return {settings.height * 16 / 9, settings.height, settings.fps, floor, bitrate, start};
}

const char* PlaybackSectionTitle(PlaybackSection section)
{
    switch (section)
    {
    case PlaybackSection::Controller: return "Manette";
    case PlaybackSection::Advanced: return "Réglages avancés";
    default: return "Général";
    }
}

std::vector<PlaybackChoice> PlaybackChoices(const PlaybackPreferences& settings, bool decoder_choice,
                                            bool filter_choice, bool fullscreen_choice)
{
    std::vector<PlaybackChoice> choices;
    auto section = PlaybackSection::General;
    const auto append = [&]<class T>(const char* title, T PlaybackPreferences::*field,
                                    std::vector<T> values, std::vector<std::string> labels) {
        const auto selected = std::ranges::find(values, settings.*field) - values.begin();
        choices.push_back({section, Tr(title), std::move(labels), static_cast<std::size_t>(selected),
            [field, values = std::move(values)](PlaybackPreferences& value, std::size_t index) {
                value.*field = values.at(index);
            }});
    };
    const auto numbers = [&](const char* label, int PlaybackPreferences::*field, const auto& values, const char* unit) {
        std::vector<std::string> labels;
        for (auto n : values) labels.push_back(std::to_string(n) + unit);
        append(label, field, std::vector<int>(values.begin(), values.end()), std::move(labels));
    };
    const auto toggle = [&](const char* label, bool PlaybackPreferences::*field, const char* off, const char* on) {
        append(label, field, {false, true}, {Tr(off), Tr(on)});
    };
    // Resolution, frame rate, bitrate and mode show the values the automatic
    // mode uses on this device. Changing one keeps the others as they are
    // and leaves the automatic mode.
    const auto effective = ResolvedPlaybackPreferences(settings);
    const auto video = [&]<class T>(const char* title, T PlaybackPreferences::*field,
                                   std::vector<T> values, std::vector<std::string> labels) {
        const auto selected = std::ranges::find(values, effective.*field) - values.begin();
        choices.push_back({section, Tr(title), std::move(labels), static_cast<std::size_t>(selected),
            [field, values = std::move(values)](PlaybackPreferences& value, std::size_t index) {
                value = ResolvedPlaybackPreferences(value);
                value.profile = QualityProfile::None;
                value.*field = values.at(index);
            }});
    };
    const auto video_numbers = [&](const char* label, int PlaybackPreferences::*field, const auto& values,
                                   const char* unit) {
        std::vector<std::string> labels;
        for (auto n : values) labels.push_back(std::to_string(n) + unit);
        video(label, field, std::vector<int>(values.begin(), values.end()), std::move(labels));
    };

    std::vector<std::string> codes{""}, names{Tr("Automatique")};
    for (const auto& language : UiLanguageOptions())
    {
        codes.emplace_back(language.code);
        names.emplace_back(language.name);
    }
    append("Langue", &PlaybackPreferences::language, std::move(codes), std::move(names));
    codes = {""};
    names = {Tr("Automatique")};
    for (const auto& market : luna::AmazonMarkets())
    {
        codes.push_back(market.code);
        names.push_back(market.native_name);
    }
    append("Compte Amazon", &PlaybackPreferences::market, std::move(codes), std::move(names));
    toggle("Affichage de la bibliothèque", &PlaybackPreferences::simple_library, "Jaquettes", "Liste simple");
    if (fullscreen_choice)
        toggle("Affichage", &PlaybackPreferences::fullscreen, "Fenêtre", "Plein écran");
    // One row for the H.264 profile, picture and sound: Automatique, the four
    // profiles, or Personnalisé once a technical row was changed.
    {
        const std::size_t selected = settings.profile != QualityProfile::None ? static_cast<std::size_t>(settings.profile) :
                                     settings.automatic ? 0u : 5u;
        choices.push_back({section, Tr("Profil de qualité"),
            {Tr("Automatique"), Tr("Faible (Baseline)"), Tr("Moyen (Main)"), Tr("Élevé (High)"), Tr("Ultra (High)"),
             Tr("Personnalisé")},
            selected, [](PlaybackPreferences& value, std::size_t index) {
                if (index >= 1 && index <= 4)
                {
                    value.profile = static_cast<QualityProfile>(index);
                    value.automatic = false;
                    return;
                }
                // Custom starts from the values in use.
                value = ResolvedPlaybackPreferences(value);
                value.profile = QualityProfile::None;
                value.automatic = index == 0;
            }});
    }
    toggle("Son", &PlaybackPreferences::audio, "Désactivé", "Activé");
    numbers("Volume", &PlaybackPreferences::volume, Volumes, " %");

    section = PlaybackSection::Controller;
    toggle("Intervertir A/B", &PlaybackPreferences::swap_ab, "Non", "Oui");
    toggle("Intervertir X/Y", &PlaybackPreferences::swap_xy, "Non", "Oui");
    toggle("Inverser l’axe Y du joystick gauche", &PlaybackPreferences::invert_left_y, "Non", "Oui");
    toggle("Clavier virtuel", &PlaybackPreferences::virtual_keyboard, "Désactivé", "Activé");

    section = PlaybackSection::Advanced;
    video_numbers("Résolution", &PlaybackPreferences::height, PlaybackHeights, "p");
    video_numbers("Images par seconde", &PlaybackPreferences::fps, PlaybackFrameRates, " FPS");
    video_numbers("Débit vidéo maximal", &PlaybackPreferences::mbps, PlaybackBitrates, " Mbps");
    std::vector<std::string> priorities;
    for (const auto mode : PlaybackPriorities) priorities.push_back(Tr(PlaybackPriorityLabel(mode)));
    video("Mode de débit", &PlaybackPreferences::priority,
        std::vector<PlaybackPriority>(PlaybackPriorities.begin(), PlaybackPriorities.end()), std::move(priorities));
    if (decoder_choice)
        append("Décodage vidéo", &PlaybackPreferences::decoder,
            std::vector<PlaybackDecoder>(Decoders.begin(), Decoders.end()),
            {Tr("Automatique"), Tr("Matériel"), Tr("Logiciel")});
    if (filter_choice)
        append("Filtre d’image", &PlaybackPreferences::filter,
            std::vector<PlaybackFilter>(Filters.begin(), Filters.end()),
            {Tr("Désactivé"), Tr("Standard"), Tr("Renforcé")});
    append("Région du serveur", &PlaybackPreferences::region,
        std::vector<std::string>(Regions.begin(), Regions.end()), std::vector<std::string>(Regions.begin(), Regions.end()));
    numbers("Tampon audio", &PlaybackPreferences::buffer_ms, Buffers, " ms");
    toggle("Infos techniques en jeu", &PlaybackPreferences::diagnostics, "Désactivé", "Activé");
    return choices;
}
}
