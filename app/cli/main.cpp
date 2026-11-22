// SPDX-License-Identifier: GPL-3.0-only

#include "../desktop/runtime.hpp"
#include "core/i18n.hpp"
#include "core/playback_preferences.hpp"
#include "core/update_check.hpp"
#include "ui/device_qr.hpp"
#include "luna/amazon_auth.hpp"
#include "luna/luna_client.hpp"
#ifdef LUNACORE_MACOS_PLAYER
#include "apple/macos_player.hpp"
#endif

#include <algorithm>
#include <charconv>
#include <csignal>
#include <filesystem>
#include <iostream>
#include <set>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
using namespace lunanx::luna;
using lunaone::desktop::AccountLock;
using lunaone::desktop::CurlRuntime;

volatile std::sig_atomic_t interrupted = 0;

void Interrupt(int)
{
    interrupted = 1;
}

void PrintUsage(std::ostream& out)
{
    out << "Usage: lunaone <command>\n"
        << "  markets          List market codes supported by the client\n"
        << "  login <market>   Sign in through Amazon's device confirmation page\n"
        << "  catalog [--all]  Fetch Luna's shelves or explore linked collections\n"
        << "  search <title>   Search the signed-in Luna catalog\n"
#ifdef LUNACORE_MACOS_PLAYER
        << "  play <ASIN or title> [--seconds N]   Open the native stream player\n"
#endif
        << "  logout           Sign out and clear local account data\n"
        << "  settings [height Mbps [auto|stable|max [30|60]]]   Show or set playback preferences\n"
        << "  settings --list / --set <row> <choice>   Show or change numbered options\n"
        << "  settings --check-updates   Check GitHub for the current public version\n"
        << "  --version        Print version and source attribution\n";
}

LunaClient MakeLunaClient(DeviceRegistration registration)
{
    CookieJar cookies;
    cookies.SetCookies(registration.cookies);
    return LunaClient(std::move(cookies), std::move(registration));
}

void ReportProgress(const std::string& status)
{
    std::cerr << status << '\n';
}

void Login(const std::string& market)
{
    if (std::filesystem::exists(AmazonAuth::StoragePath()))
        throw std::runtime_error("An Amazon device file already exists. Run `lunaone logout` before signing in again.");

    std::signal(SIGINT, Interrupt);
    auto registration = AmazonAuth::SignInWithCode(
        market,
        [](const DeviceCodeChallenge& challenge) {
            std::cout << "Open " << challenge.verification_url << " and enter " << challenge.user_code
                      << "\nWaiting for Amazon confirmation. The code expires in "
                      << challenge.expires_in_seconds << " seconds.\n" << std::flush;
            const auto qr = lunanx::ui::MakeDeviceQr(challenge.verification_url);
            for (int y = 0; y < qr.size; y += 2)
            {
                std::cout << "\x1b[30;107m";
                for (int x = 0; x < qr.size; ++x)
                {
                    const bool upper = qr.dark[y * qr.size + x] != 0;
                    const bool lower = y + 1 < qr.size && qr.dark[(y + 1) * qr.size + x] != 0;
                    std::cout << (upper ? (lower ? "█" : "▀") : (lower ? "▄" : " "));
                }
                std::cout << "\x1b[0m\n";
            }
            std::cout.flush();
        },
        [] { return interrupted != 0; });

    try
    {
        if (interrupted)
            throw std::runtime_error("Sign-in cancelled.");
        if (!AmazonAuth::Save(registration))
            throw std::runtime_error("Could not save the Amazon device.");
    }
    catch (...)
    {
        try { AmazonAuth::Deregister(registration); } catch (...) {}
        throw;
    }

    std::cerr << "Amazon confirmed the device registration.\n";
    auto client = MakeLunaClient(std::move(registration));
    client.SignIn(ReportProgress);
    std::cout << "Signed in to Luna for market " << market << ".\n";
}

LunaClient LoadLunaClient()
{
    DeviceRegistration registration;
    if (!AmazonAuth::Load(registration))
        throw std::runtime_error("No saved Amazon device. Run `lunaone login <market>`.");
    return MakeLunaClient(std::move(registration));
}

void PrintGame(const Game& game)
{
    std::cout << "  " << game.title;
    if (!game.asin.empty())
        std::cout << " [" << game.asin << ']';
    std::cout << '\n';
}

void Catalog(bool expand)
{
    std::signal(SIGINT, Interrupt);
    auto client = LoadLunaClient();
    client.SignIn(ReportProgress);

    const auto shelves = client.FetchShelves();
    std::size_t count = 0;
    std::set<std::string> known;
    for (const auto& shelf : shelves)
    {
        if (shelf.games.empty())
            continue;
        if (!expand)
            std::cout << '\n' << shelf.title << '\n';
        for (const auto& game : shelf.games)
        {
            if (expand)
            {
                if (!InsertGameKeys(known, game))
                    continue;
                PrintGame(game);
            }
            else
                std::cout << "  " << game.title << '\n';
            ++count;
        }
    }
    if (expand)
    {
        client.ExploreCatalog(std::move(known), [&](std::vector<Game> games) {
            for (const auto& game : games)
                PrintGame(game);
            count += games.size();
        }, [] { return !interrupted; });
        std::cout << '\n' << count << " distinct games found.\n";
        return;
    }
    std::cout << '\n' << count << " catalog entries across " << shelves.size() << " shelves.\n";
}

void Search(const std::string& query)
{
    std::signal(SIGINT, Interrupt);
    auto client = LoadLunaClient();
    client.SignIn(ReportProgress);
    const auto games = client.SearchGames(query, [] { return !interrupted; });
    for (const auto& game : games)
        PrintGame(game);
    std::cout << games.size() << " Luna search results.\n";
}

#ifdef LUNACORE_MACOS_PLAYER
void Play(const std::string& query, int seconds)
{
    std::signal(SIGINT, Interrupt);
    auto client = LoadLunaClient();
    client.SignIn(ReportProgress);
    const auto expected_title = NormalizedGameTitle(query);
    const auto matches = [&](const Game& game) {
        return game.asin == query || (!expected_title.empty() && NormalizedGameTitle(game.title) == expected_title);
    };
    for (const auto& shelf : client.FetchShelves())
    {
        if (const auto game = std::ranges::find_if(shelf.games, matches); game != shelf.games.end())
        {
            if (!interrupted)
                lunaone::apple::Play(client, *game, std::chrono::seconds(seconds), [] { return interrupted != 0; });
            return;
        }
    }
    const auto results = client.SearchGames(query, [] { return !interrupted; });
    if (interrupted)
        return;
    if (const auto game = std::ranges::find_if(results, matches); game != results.end())
    {
        lunaone::apple::Play(client, *game, std::chrono::seconds(seconds), [] { return interrupted != 0; });
        return;
    }
    throw std::runtime_error("Game not found in the signed-in catalog. Use an exact title or ASIN.");
}
#endif

void Logout()
{
    DeviceRegistration registration;
    if (AmazonAuth::Load(registration))
    {
        try { AmazonAuth::Deregister(registration); }
        catch (const std::exception&)
        {
            std::cerr << "Could not reach Amazon to remove the device. Local sign-out will continue.\n";
        }
    }
    if (!AmazonAuth::EraseAccountData())
        throw std::runtime_error("Could not remove all local account data.");
    std::cout << "Local account data removed.\n";
}
} // namespace

static int Run(int argc, const char* const* argv)
{
    const std::string_view command = argc > 1 ? argv[1] : "--help";
    if (argc <= 2 && (command == "--help" || command == "help"))
    {
        PrintUsage(std::cout);
        return 0;
    }
    if (argc == 2 && command == "--version")
    {
        std::cout << "LunaONE " << LUNACORE_VERSION << " (GPL-3.0)\n"
                     "Derived from LunaNX 3.1 by KazaR-RooT. See LICENSE and NOTICE.\n";
        return 0;
    }
    if (argc == 2 && command == "markets")
    {
        for (const auto& market : AmazonMarkets())
            std::cout << market.code << '\t' << market.native_name << '\n';
        return 0;
    }

    const bool login = argc == 3 && command == "login";
    const bool search = argc == 3 && command == "search" && argv[2][0] != '\0';
    const bool catalog = command == "catalog" && (argc == 2 || (argc == 3 && std::string_view(argv[2]) == "--all"));
    const bool check_updates = command == "settings" && argc == 3 && std::string_view(argv[2]) == "--check-updates";
    const bool list_settings = command == "settings" && argc == 3 && std::string_view(argv[2]) == "--list";
    const bool change_setting = command == "settings" && argc == 5 && std::string_view(argv[2]) == "--set";
    const bool settings = command == "settings" && (argc == 2 || argc == 4 || argc == 5 || argc == 6);
    bool play = false;
#ifdef LUNACORE_MACOS_PLAYER
    int seconds = 0;
    play = command == "play" && (argc == 3 || (argc == 5 && std::string_view(argv[3]) == "--seconds"));
    if (play && argc == 5)
    {
        const std::string_view text(argv[4]);
        const auto [end, error] = std::from_chars(text.begin(), text.end(), seconds);
        if (error != std::errc{} || end != text.end() || seconds < 1 || seconds > 3600)
        {
            std::cerr << "Seconds must be between 1 and 3600.\n";
            return 2;
        }
    }
#endif
    if (!login && !play && !search && !catalog && !settings && !list_settings && !check_updates && !(argc == 2 && command == "logout"))
    {
        PrintUsage(std::cerr);
        return 2;
    }
    if (login && std::ranges::none_of(AmazonMarkets(), [code = std::string_view(argv[2])](const auto& market) {
            return market.code == code;
        }))
    {
        std::cerr << "Unknown market. Run `lunaone markets`.\n";
        return 2;
    }

    try
    {
        if (check_updates)
        {
            const CurlRuntime curl;
            std::cout << lunanx::CheckForUpdates() << '\n';
            return 0;
        }
        const AccountLock lock;
        if (settings || list_settings)
        {
            auto options = lunanx::LoadPlaybackPreferences();
            const auto integer = [](std::string_view text, int& value) {
                const auto end = text.data() + text.size();
                const auto result = std::from_chars(text.data(), end, value);
                return result.ec == std::errc{} && result.ptr == end;
            };
            if (list_settings || change_setting)
            {
                const auto choices = lunanx::PlaybackChoices(options);
                if (change_setting)
                {
                    int row, index;
                    if (!integer(argv[3], row) || !integer(argv[4], index) || row < 1 || index < 1 ||
                        static_cast<std::size_t>(row) > choices.size() ||
                        static_cast<std::size_t>(index) > choices[row - 1].labels.size())
                    {
                        std::cerr << "Use settings --list to see valid row and choice numbers.\n";
                        return 2;
                    }
                    const auto& choice = choices[row - 1];
                    choice.apply(options, index - 1);
                    if (!lunanx::SavePlaybackPreferences(options))
                        throw std::runtime_error("Could not save playback settings.");
                    std::cout << choice.title << "  " << choice.labels[index - 1] << '\n';
                }
                else
                    for (std::size_t i = 0; i < choices.size(); ++i)
                    {
                        const auto& choice = choices[i];
                        std::cout << i + 1 << ". " << choice.title << '\n';
                        for (std::size_t j = 0; j < choice.labels.size(); ++j)
                            std::cout << (j == choice.selected ? " * " : "   ") << j + 1 << ". " << choice.labels[j] << '\n';
                    }
                return 0;
            }
            if (argc >= 5)
            {
                const std::string_view priority = argv[4];
                // The former priority names stay accepted for existing scripts.
                if (priority == "auto" || priority == "adaptive" || priority == "clarity" || priority == "original")
                    options.priority = lunanx::PlaybackPriority::Automatic;
                else if (priority == "stable" || priority == "smooth")
                    options.priority = lunanx::PlaybackPriority::Stable;
                else if (priority == "max")
                    options.priority = lunanx::PlaybackPriority::Maximum;
                else
                {
                    std::cerr << "Choose auto, stable or max.\n";
                    return 2;
                }
            }
            if (argc == 6 && !integer(argv[5], options.fps))
            {
                std::cerr << "Choose 30 or 60 FPS.\n";
                return 2;
            }
            if (argc >= 4 && (!integer(argv[2], options.height) || !integer(argv[3], options.mbps) ||
                              !lunanx::ValidPlaybackPreferences(options)))
            {
                std::cerr << "Choose 720, 1080, 1440 or 2160 and 5, 10, 15, 20 or 25 Mbps, with 30 or 60 FPS.\n";
                return 2;
            }
            // Explicit values leave the automatic mode.
            if (argc >= 4) options.automatic = false;
            if (argc >= 4 && !lunanx::SavePlaybackPreferences(options))
                throw std::runtime_error("Could not save playback settings.");
            const auto shown = lunanx::ResolvedPlaybackPreferences(options);
            std::cout << (options.automatic ? lunanx::Tr("Automatique") + " · " : std::string())
                      << shown.height << "p · " << shown.mbps << " Mbps · "
                      << lunanx::Tr(lunanx::PlaybackPriorityLabel(shown.priority)) << " · " << shown.fps << " FPS\n";
            return 0;
        }
        const CurlRuntime curl;
        if (login)
            Login(argv[2]);
#ifdef LUNACORE_MACOS_PLAYER
        else if (play)
            Play(argv[2], seconds);
#endif
        else if (catalog)
            Catalog(argc == 3);
        else if (search)
            Search(argv[2]);
        else
            Logout();
        return interrupted ? 130 : 0;
    }
    catch (const std::exception& ex)
    {
        std::cerr << "LunaONE: " << ex.what() << '\n';
        return interrupted ? 130 : 1;
    }
}

#ifdef _WIN32
int wmain(int argc, wchar_t** argv)
{
    SetConsoleOutputCP(CP_UTF8);
    std::vector<std::string> arguments;
    arguments.reserve(argc);
    for (int index = 0; index < argc; ++index)
    {
        const int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, argv[index], -1, nullptr, 0, nullptr, nullptr);
        if (size == 0)
        {
            std::cerr << "Invalid Unicode command-line argument.\n";
            return 2;
        }
        std::string argument(static_cast<std::size_t>(size), '\0');
        WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, argv[index], -1, argument.data(), size, nullptr, nullptr);
        argument.pop_back();
        arguments.push_back(std::move(argument));
    }
    std::vector<const char*> pointers;
    pointers.reserve(arguments.size());
    for (const auto& argument : arguments)
        pointers.push_back(argument.c_str());
    return Run(argc, pointers.data());
}
#else
int main(int argc, char** argv)
{
    return Run(argc, argv);
}
#endif
