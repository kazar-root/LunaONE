// SPDX-License-Identifier: GPL-3.0-only
#include "update_check.hpp"
#include "http_client.hpp"
#include "i18n.hpp"

#include <jansson.h>
#include <mbedtls/sha256.h>

#include <array>
#include <charconv>
#include <cstdio>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

namespace lunanx
{
namespace
{
using Version = std::array<unsigned, 3>;
constexpr char GithubApi[] = "https://api.github.com/repos/kazar-root/LunaONE";

// "V0.1", "0.1" or "0.1.2": two or three numbers, the missing patch being 0.
std::optional<Version> ParseVersion(std::string_view text)
{
    if (text.starts_with('v') || text.starts_with('V'))
        text.remove_prefix(1);
    Version version{};
    std::size_t parts = 0;
    for (;;)
    {
        if (parts == version.size())
            return std::nullopt;
        const auto dot = text.find('.');
        const auto part = text.substr(0, dot);
        if (part.empty() || (part.size() > 1 && part.front() == '0'))
            return std::nullopt;
        const auto [end, error] = std::from_chars(part.data(), part.data() + part.size(), version[parts]);
        if (error != std::errc{} || end != part.data() + part.size())
            return std::nullopt;
        ++parts;
        if (dot == text.npos)
            break;
        text.remove_prefix(dot + 1);
    }
    if (parts < 2)
        return std::nullopt;
    return version;
}

using Json = std::unique_ptr<json_t, decltype(&json_decref)>;

Json GetGithub(const std::string& path, long& status)
{
    const auto response = HttpClient{}.Get(GithubApi + path, "LunaONE/" + std::string(InstalledVersion()),
        {"Accept: application/vnd.github+json"}, 4, 8);
    status = response.status_code;
    return {json_loadb(response.body.data(), response.body.size(), 0, nullptr), json_decref};
}

std::optional<Version> ReadVersion(json_t* object, const char* key)
{
    const auto name = json_string_value(json_object_get(object, key));
    return name ? ParseVersion(name) : std::nullopt;
}

// 0.1, or 0.1.2 when there is a patch number.
std::string DisplayVersion(const Version& version)
{
    auto text = std::to_string(version[0]) + "." + std::to_string(version[1]);
    if (version[2])
        text += "." + std::to_string(version[2]);
    return text;
}

constexpr char DownloadPrefix[] = "https://github.com/kazar-root/LunaONE/releases/download/";

// The SHA-256 that SHA256SUMS.txt gives for `name`, in lowercase hexadecimal.
std::string ExpectedSha256(const std::string& sums, const std::string& name)
{
    std::istringstream lines(sums);
    for (std::string line; std::getline(lines, line);)
    {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const auto space = line.find(' ');
        if (space != 64) continue;
        auto file = std::string_view(line).substr(space);
        while (!file.empty() && (file.front() == ' ' || file.front() == '*')) file.remove_prefix(1);
        if (file != name) continue;
        std::string hash = line.substr(0, 64);
        for (auto& c : hash)
            if (c >= 'A' && c <= 'F') c = static_cast<char>(c - 'A' + 'a');
        if (hash.find_first_not_of("0123456789abcdef") == std::string::npos) return hash;
    }
    return {};
}

// The release's file for this device, and its SHA256SUMS.txt.
void ReadAssets(json_t* release, UpdateInfo& update)
{
    const auto suffix = UpdateAssetSuffix();
    std::size_t index;
    json_t* asset;
    json_array_foreach(json_object_get(release, "assets"), index, asset)
    {
        const char* name = json_string_value(json_object_get(asset, "name"));
        const char* url = json_string_value(json_object_get(asset, "browser_download_url"));
        if (!name || !url || !std::string_view(url).starts_with(DownloadPrefix)) continue;
        const std::string_view file(name);
        if (file == "SHA256SUMS.txt")
            update.sums_url = url;
        else if (!suffix.empty() && file.starts_with("LunaONE-") && file.ends_with(suffix))
        {
            update.asset_name = name;
            update.asset_url = url;
            update.asset_size = static_cast<long long>(json_integer_value(json_object_get(asset, "size")));
            // "sha256:…", computed by GitHub when the file was uploaded; read
            // as a SHA256SUMS.txt line, which also checks its form.
            const char* digest = json_string_value(json_object_get(asset, "digest"));
            if (digest && std::string_view(digest).starts_with("sha256:"))
                update.sha256 = ExpectedSha256(std::string(digest + 7) + "  " + name, name);
        }
    }
}

std::string Sha256(const std::string& data)
{
    std::array<unsigned char, 32> digest {};
    mbedtls_sha256(reinterpret_cast<const unsigned char*>(data.data()), data.size(), digest.data(), 0);
    static constexpr char digits[] = "0123456789abcdef";
    std::string hex;
    for (const auto byte : digest)
    {
        hex += digits[byte >> 4];
        hex += digits[byte & 15];
    }
    return hex;
}
}

std::string_view InstalledVersion() { return LUNAONE_VERSION; }

std::string_view UpdateAssetSuffix()
{
#if defined(__ANDROID__)
    return "-Android-GoogleTV.apk";
#elif defined(__APPLE__) && TARGET_OS_OSX
    return "-macOS-universal.zip";
#elif defined(_WIN32) && (defined(__aarch64__) || defined(_M_ARM64))
    return "-Windows-arm64.zip";
#elif defined(_WIN32) && (defined(__x86_64__) || defined(_M_X64))
    return "-Windows-x64.zip";
#elif defined(__FreeBSD__) && defined(__aarch64__)
    return "-FreeBSD-aarch64.tar.gz";
#elif defined(__FreeBSD__) && defined(__x86_64__)
    return "-FreeBSD-amd64.tar.gz";
#elif defined(__linux__) && !defined(__GLIBC__) && defined(__aarch64__)
    return "-Linux-Alpine-arm64.tar.gz";
#else
    return {};
#endif
}

std::string VersionLabel()
{
    const auto version = ParseVersion(InstalledVersion());
    if (!version) return "V" + std::string(InstalledVersion());
    std::string label = "V" + std::to_string((*version)[0]) + "." + std::to_string((*version)[1]);
    if ((*version)[2]) label += "." + std::to_string((*version)[2]);
    return label;
}

// The message for the installed version, and what the latest release offers.
static std::string CheckRelease(std::string_view installed, UpdateInfo& update)
{
    const std::string current = Tr("Version installée") + "  " + std::string(installed) + "\n";
    const std::string unavailable = current + Tr("Impossible de vérifier les mises à jour sur GitHub.");
    try
    {
        const auto local = ParseVersion(installed);
        if (!local)
            return unavailable;
        long status = 0;
        const auto release = GetGithub("/releases/latest", status);
        if (status != 200 && status != 404)
            return unavailable;
        if (status == 200 && !json_is_object(release.get()))
            return unavailable;
        auto remote = ReadVersion(release.get(), "tag_name");
        if (remote && *remote > *local)
        {
            update.available = true;
            update.version = DisplayVersion(*remote);
            ReadAssets(release.get(), update);
        }
        if (!remote)
        {
            const auto tags = GetGithub("/tags?per_page=100", status);
            if (status != 200 || !json_is_array(tags.get()))
                return unavailable;
            std::size_t index;
            json_t* tag;
            json_array_foreach(tags.get(), index, tag)
            {
                const auto version = ReadVersion(tag, "name");
                if (version && (!remote || *version > *remote))
                    remote = version;
            }
        }
        if (!remote)
            return current + Tr("Aucune version stable publiée sur GitHub.");
        if (*remote > *local && !update.available)
        {
            // Only a tag, without a release and its files.
            update.available = true;
            update.version = DisplayVersion(*remote);
        }
        return current + Tr(*remote > *local ? "Mise à jour disponible" :
                            *remote < *local ? "Version installée plus récente que la version publique" :
                                               "LunaONE est à jour") +
            " (" + DisplayVersion(*remote) + ")";
    }
    catch (const std::exception&)
    {
        return unavailable;
    }
}

std::string CheckForUpdates(std::string_view installed) { return FindUpdate(installed).message; }

UpdateInfo FindUpdate(std::string_view installed)
{
    UpdateInfo update;
    update.message = CheckRelease(installed, update);
    return update;
}

std::string DownloadUpdate(const UpdateInfo& update)
{
    if (!update.Downloadable())
        throw std::runtime_error(Tr("Télécharge la nouvelle version sur GitHub."));
    const std::string agent = "LunaONE/" + std::string(InstalledVersion());
    std::string expected = update.sha256;
    HttpResponse file;
    try
    {
        if (expected.empty())
        {
            const auto sums = HttpClient{}.Get(update.sums_url, agent, {}, 10, 60);
            if (sums.status_code == 200)
                expected = ExpectedSha256(sums.body, update.asset_name);
        }
        if (!expected.empty())
            file = HttpClient{}.Get(update.asset_url, agent, {}, 10, 600);
    }
    catch (const std::exception&)
    {
        throw std::runtime_error(Tr("Impossible de télécharger la mise à jour."));
    }
    if (expected.empty() || file.status_code != 200)
        throw std::runtime_error(Tr("Impossible de télécharger la mise à jour."));
    if ((update.asset_size > 0 && static_cast<long long>(file.body.size()) != update.asset_size) ||
        Sha256(file.body) != expected)
        throw std::runtime_error(Tr("Le fichier téléchargé ne correspond pas à la version publiée."));
    return std::move(file.body);
}

}
