// LunaNX-Origin: KazaR-RooT | urn:uuid:57e9877e-564c-4275-ae2a-a70f276bfbed | unit=23c239828debbf16bc21
// Project origin: https://github.com/kazar-root/LunaNX (third-party notices remain applicable).
#include "core/source_origin.hpp"
LUNANX_SOURCE_ORIGIN(u23c239828debbf16bc21, "23c239828debbf16bc21")

#include "app_paths.hpp"

#include <cstdlib>
#include <filesystem>
#include <stdexcept>

#ifndef _WIN32
#include <cerrno>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace lunanx
{
namespace
{
namespace fs = std::filesystem;

fs::path DataDirectory(const fs::path& parent)
{
    const auto current = parent / "LunaONE";
    const auto previous = parent / "LunaCore";
    return !fs::exists(current) && fs::exists(previous) ? previous : current;
}

// Adapted for LunaCore on 2026-11-18 to use native desktop data directories.
fs::path ResolveAppHome()
{
    const char* override_path = std::getenv("LUNAONE_DATA_DIR");
    if (!override_path || !*override_path)
        override_path = std::getenv("LUNACORE_DATA_DIR");
    if (override_path && *override_path)
    {
        auto path = fs::absolute(override_path).lexically_normal();
        if (!path.has_filename() && path.has_relative_path())
            path = path.parent_path();
        return path;
    }

#if defined(_WIN32)
    if (const char* app_data = std::getenv("LOCALAPPDATA"); app_data && *app_data)
        return DataDirectory(app_data);
#else
    const char* home = std::getenv("HOME");
#if defined(__APPLE__)
    if (home && *home)
        return DataDirectory(fs::path(home) / "Library" / "Application Support");
#else
    if (const char* data = std::getenv("XDG_DATA_HOME"); data && fs::path(data).is_absolute())
        return DataDirectory(data);
    if (home && *home)
        return DataDirectory(fs::path(home) / ".local" / "share");
#endif
#endif
    throw std::runtime_error("Cannot locate the LunaONE data directory. Set LUNAONE_DATA_DIR.");
}

} // namespace

const std::string& AppHomePath()
{
    static const std::string path = ResolveAppHome().string();
    return path;
}

void PrepareAppStorage()
{
    const fs::path path(AppHomePath());
#ifdef _WIN32
    fs::create_directories(path);
#else
    fs::create_directories(path.parent_path());
    if (mkdir(path.c_str(), 0700) != 0 && errno != EEXIST)
        throw std::runtime_error("Cannot create the LunaONE data directory.");
    struct stat info {};
    if (lstat(path.c_str(), &info) != 0 || !S_ISDIR(info.st_mode) ||
        info.st_uid != geteuid() || (info.st_mode & 0077) != 0)
        throw std::runtime_error("The LunaONE data directory must be private and owned by the current user.");
#endif
}

} // namespace lunanx
