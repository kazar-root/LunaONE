// SPDX-License-Identifier: GPL-3.0-only
#include "update_check.hpp"
#include "http_client.hpp"
#include "i18n.hpp"

#include <jansson.h>

#include <array>
#include <charconv>
#include <memory>
#include <optional>

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
}

std::string_view InstalledVersion() { return LUNAONE_VERSION; }

std::string VersionLabel()
{
    const auto version = ParseVersion(InstalledVersion());
    if (!version) return "V" + std::string(InstalledVersion());
    std::string label = "V" + std::to_string((*version)[0]) + "." + std::to_string((*version)[1]);
    if ((*version)[2]) label += "." + std::to_string((*version)[2]);
    return label;
}

std::string CheckForUpdates(std::string_view installed)
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
}
