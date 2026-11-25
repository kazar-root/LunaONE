// SPDX-License-Identifier: GPL-3.0-only
#include "self_update.hpp"
#include "i18n.hpp"

#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

// Where LunaONE replaces itself: macOS, Windows, Linux and FreeBSD.
#if defined(_WIN32)
#define LUNAONE_SELF_UPDATE 1
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#include <cwchar>
#elif !defined(__ANDROID__) && !(defined(__APPLE__) && !TARGET_OS_OSX)
#define LUNAONE_SELF_UPDATE 1
#include <cerrno>
#include <climits>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#elif defined(__FreeBSD__)
#include <sys/types.h>
#include <sys/sysctl.h>
#endif
extern char** environ;
#endif

namespace lunanx
{
#if LUNAONE_SELF_UPDATE
namespace
{
namespace fs = std::filesystem;

[[noreturn]] void Fail()
{
    throw std::runtime_error(Tr("Impossible d’installer la mise à jour à cet emplacement."));
}

fs::path ExecutablePath()
{
#if defined(_WIN32)
    std::wstring path(32768, L'\0');
    const DWORD size = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (size == 0 || size >= path.size()) return {};
    path.resize(size);
    return path;
#elif defined(__APPLE__)
    uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::string path(size, '\0');
    if (_NSGetExecutablePath(path.data(), &size) != 0) return {};
    std::error_code error;
    auto real = fs::canonical(path.c_str(), error);
    return error ? fs::path {} : real;
#elif defined(__FreeBSD__)
    int mib[] {CTL_KERN, KERN_PROC, KERN_PROC_PATHNAME, -1};
    char path[PATH_MAX] {};
    std::size_t size = sizeof path;
    if (sysctl(mib, 4, path, &size, nullptr, 0) != 0 || !path[0]) return {};
    return path;
#else
    std::error_code error;
    auto path = fs::read_symlink("/proc/self/exe", error);
    return error ? fs::path {} : path;
#endif
}

// What the update replaces: the app bundle on a Mac, the program's folder elsewhere.
fs::path InstallTarget()
{
    const auto program = ExecutablePath();
    if (program.empty()) return {};
#if defined(__APPLE__)
    // …/LunaONE.app/Contents/MacOS/LunaONE
    auto bundle = program.parent_path().parent_path().parent_path();
    return bundle.extension() == ".app" ? bundle : fs::path {};
#else
    return program.parent_path();
#endif
}

#if defined(_WIN32)
constexpr wchar_t ProgramName[] = L"LunaONE.exe";
constexpr wchar_t ReplacedSuffix[] = L".lunaone-old";
#else
constexpr char ProgramName[] = "LunaONE";
#endif

bool Writable(const fs::path& folder)
{
#if defined(_WIN32)
    const auto probe = folder / (L".lunaone-write-" + std::to_wstring(GetCurrentProcessId()));
    const HANDLE file = CreateFileW(probe.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                    FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    CloseHandle(file);
    return true;
#else
    return access(folder.c_str(), W_OK) == 0;
#endif
}

// The update is unpacked next to what it replaces, so that moving files
// never crosses from one disk to another.
fs::path WorkFolder(const fs::path& target)
{
#if defined(__APPLE__)
    return target.parent_path() / (".LunaONE-update-" + std::to_string(getpid()));
#elif defined(_WIN32)
    return target / (L".lunaone-update-" + std::to_wstring(GetCurrentProcessId()));
#else
    return target / (".lunaone-update-" + std::to_string(getpid()));
#endif
}

#if defined(_WIN32)
// Waits for a program started with CreateProcessW and returns its exit code.
DWORD RunHidden(const std::wstring& program, std::wstring command, const fs::path& folder = {})
{
    STARTUPINFOW startup {};
    startup.cb = sizeof startup;
    PROCESS_INFORMATION process {};
    if (!CreateProcessW(program.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                        folder.empty() ? nullptr : folder.c_str(), &startup, &process))
        Fail();
    WaitForSingleObject(process.hProcess, 180000);
    DWORD code = 1;
    GetExitCodeProcess(process.hProcess, &code);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return code;
}

// Windows 10 and 11 ship tar.exe, which reads zip archives.
void Extract(const fs::path& archive, const fs::path& folder)
{
    wchar_t system[MAX_PATH] {};
    const UINT length = GetSystemDirectoryW(system, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) Fail();
    const std::wstring tar = std::wstring(system) + L"\\tar.exe";
    if (RunHidden(tar, L"\"" + tar + L"\" -xf \"" + archive.wstring() + L"\" -C \"" + folder.wstring() + L"\"") != 0)
        Fail();
}
#else
void Run(const std::vector<std::string>& arguments)
{
    std::vector<char*> argv;
    for (const auto& argument : arguments) argv.push_back(const_cast<char*>(argument.c_str()));
    argv.push_back(nullptr);
    pid_t child = 0;
    if (posix_spawnp(&child, argv[0], nullptr, nullptr, argv.data(), environ) != 0) Fail();
    int status = 0;
    pid_t done = 0;
    while ((done = waitpid(child, &status, 0)) < 0 && errno == EINTR) {}
    if (done == child && (!WIFEXITED(status) || WEXITSTATUS(status) != 0)) Fail();
}

void Extract(const fs::path& archive, const fs::path& folder)
{
#if defined(__APPLE__)
    Run({"/usr/bin/ditto", "-x", "-k", archive.string(), folder.string()});
#else
    Run({"tar", "-xzf", archive.string(), "-C", folder.string()});
#endif
}
#endif

// The unpacked app: LunaONE.app on a Mac, the LunaONE-V… folder elsewhere.
fs::path Payload(const fs::path& folder)
{
#if defined(__APPLE__)
    const auto app = folder / "LunaONE.app";
    return fs::is_regular_file(app / "Contents" / "MacOS" / "LunaONE") ? app : fs::path {};
#else
    for (const auto& entry : fs::directory_iterator(folder))
        if (entry.is_directory() && entry.path().filename().string().starts_with("LunaONE-V") &&
            fs::is_regular_file(entry.path() / ProgramName))
            return entry.path();
    return {};
#endif
}

#if defined(__APPLE__)
// The running bundle moves aside and the new one takes its place.
void Replace(const fs::path& payload, const fs::path& bundle, const fs::path& work)
{
    const auto previous = work / "previous.app";
    std::error_code error;
    fs::rename(bundle, previous, error);
    if (error) Fail();
    fs::rename(payload, bundle, error);
    if (error)
    {
        fs::rename(previous, bundle, error);
        Fail();
    }
}
#elif defined(_WIN32)
// Windows lets a running program and its DLLs be renamed but not overwritten:
// each file moves aside, the new one is copied in, and the new version removes
// the old files once this one has ended. Any failure puts everything back.
void Replace(const fs::path& payload, const fs::path& folder, const fs::path&)
{
    std::vector<std::pair<fs::path, fs::path>> moved;
    std::vector<fs::path> added;
    std::error_code ignored;
    try
    {
        for (const auto& entry : fs::recursive_directory_iterator(payload))
        {
            if (!entry.is_regular_file()) continue;
            const auto target = folder / fs::relative(entry.path(), payload);
            fs::create_directories(target.parent_path());
            if (fs::exists(target))
            {
                auto old = target;
                old += ReplacedSuffix;
                fs::remove(old, ignored);
                fs::rename(target, old);
                moved.emplace_back(old, target);
            }
            else
                added.push_back(target);
            fs::copy_file(entry.path(), target, fs::copy_options::overwrite_existing);
        }
    }
    catch (const std::exception&)
    {
        for (const auto& file : added) fs::remove(file, ignored);
        for (auto it = moved.rbegin(); it != moved.rend(); ++it)
        {
            fs::remove(it->second, ignored);
            fs::rename(it->first, it->second, ignored);
        }
        Fail();
    }
}
#else
// Every new file is copied beside its old one first, then renamed over it:
// a rename replaces even the running program without disturbing it.
void Replace(const fs::path& payload, const fs::path& folder, const fs::path&)
{
    std::vector<std::pair<fs::path, fs::path>> copies;
    std::error_code ignored;
    try
    {
        for (const auto& entry : fs::recursive_directory_iterator(payload))
        {
            if (!entry.is_regular_file()) continue;
            const auto target = folder / fs::relative(entry.path(), payload);
            if (fs::create_directories(target.parent_path()))
                fs::permissions(target.parent_path(), fs::perms(0755));
            auto fresh = target;
            fresh += ".lunaone-new";
            fs::copy_file(entry.path(), fresh, fs::copy_options::overwrite_existing);
            // The release's modes, whatever this process's umask made of them.
            const bool program = (fs::status(entry.path()).permissions() & fs::perms::owner_exec) != fs::perms::none;
            fs::permissions(fresh, fs::perms(program ? 0755 : 0644));
            copies.emplace_back(fresh, target);
        }
    }
    catch (const std::exception&)
    {
        for (const auto& copy : copies) fs::remove(copy.first, ignored);
        Fail();
    }
    for (const auto& [fresh, target] : copies)
    {
        std::error_code error;
        fs::rename(fresh, target, error);
        if (error) Fail();
    }
}
#endif

// Starts the new version once this process has ended.
void Restart(const fs::path& target, const fs::path& work)
{
#if defined(_WIN32)
    (void)work;
    const auto program = target / ProgramName;
    std::wstring command = L"\"" + program.wstring() + L"\" --after-update " + std::to_wstring(GetCurrentProcessId());
    STARTUPINFOW startup {};
    startup.cb = sizeof startup;
    PROCESS_INFORMATION process {};
    if (!CreateProcessW(program.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr, target.c_str(), &startup,
                        &process))
        Fail();
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
#else
#if defined(__APPLE__)
    const std::string script =
        "while kill -0 \"$1\" 2>/dev/null; do sleep 0.2; done; rm -rf \"$3\"; exec /usr/bin/open \"$2\"";
    const std::string program = target.string();
#else
    const std::string script = "while kill -0 \"$1\" 2>/dev/null; do sleep 0.2; done; rm -rf \"$3\"; exec \"$2\"";
    const std::string program = (target / ProgramName).string();
#endif
    std::vector<std::string> arguments {"/bin/sh", "-c", script, "lunaone-update", std::to_string(getpid()), program,
                                        work.string()};
    std::vector<char*> argv;
    for (const auto& argument : arguments) argv.push_back(const_cast<char*>(argument.c_str()));
    argv.push_back(nullptr);
    posix_spawnattr_t attributes;
    posix_spawnattr_init(&attributes);
#ifdef POSIX_SPAWN_SETSID
    // Its own session: it survives this process and the terminal it came from.
    posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETSID);
#endif
    pid_t child = 0;
    const int result = posix_spawn(&child, "/bin/sh", nullptr, &attributes, argv.data(), environ);
    posix_spawnattr_destroy(&attributes);
    if (result != 0) Fail();
#endif
}
} // namespace
#endif

bool CanInstallUpdate(const UpdateInfo& update)
{
#if LUNAONE_SELF_UPDATE
    if (!update.Downloadable()) return false;
    const auto target = InstallTarget();
    if (target.empty()) return false;
#if defined(__APPLE__)
    // Moving the bundle needs write access to it and to its folder; a
    // translocated or read-only copy has neither.
    return Writable(target.parent_path()) && Writable(target);
#else
    return Writable(target);
#endif
#else
    (void)update;
    return false;
#endif
}

void InstallUpdate(const UpdateInfo& update)
{
#if LUNAONE_SELF_UPDATE
    if (!CanInstallUpdate(update)) Fail();
    const auto data = DownloadUpdate(update);
    const auto target = InstallTarget();
    const auto work = WorkFolder(target);
    std::error_code ignored;
    fs::remove_all(work, ignored);
    try
    {
        fs::create_directories(work / "files");
        const auto archive = work / fs::path(update.asset_name).filename();
        {
            std::ofstream file(archive, std::ios::binary);
            file.write(data.data(), static_cast<std::streamsize>(data.size()));
            if (!file) Fail();
        }
        Extract(archive, work / "files");
        const auto payload = Payload(work / "files");
        if (payload.empty()) Fail();
        Replace(payload, target, work);
        Restart(target, work);
    }
    catch (const fs::filesystem_error&)
    {
        fs::remove_all(work, ignored);
        Fail();
    }
    catch (...)
    {
        fs::remove_all(work, ignored);
        throw;
    }
#if !defined(__APPLE__)
    // The Mac helper removes the old bundle once this process has ended.
    fs::remove_all(work, ignored);
#endif
#else
    (void)update;
    throw std::runtime_error(Tr("Télécharge la nouvelle version sur GitHub."));
#endif
}

void CompletePendingUpdate()
{
#if defined(_WIN32)
    int count = 0;
    wchar_t** arguments = CommandLineToArgvW(GetCommandLineW(), &count);
    if (!arguments) return;
    DWORD previous = 0;
    for (int i = 1; i + 1 < count; ++i)
        if (std::wstring_view(arguments[i]) == L"--after-update")
            previous = static_cast<DWORD>(std::wcstoul(arguments[i + 1], nullptr, 10));
    LocalFree(arguments);
    if (!previous) return;
    if (const HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, previous))
    {
        WaitForSingleObject(process, 20000);
        CloseHandle(process);
    }
    const auto folder = InstallTarget();
    if (folder.empty()) return;
    std::vector<fs::path> replaced;
    std::error_code error;
    for (auto it = fs::recursive_directory_iterator(folder, error); !error && it != fs::recursive_directory_iterator();
         it.increment(error))
        if (it->path().extension() == ReplacedSuffix) replaced.push_back(it->path());
    // The old program may need a moment to let go of its files.
    for (int attempt = 0; attempt < 10 && !replaced.empty(); ++attempt)
    {
        std::vector<fs::path> left;
        for (const auto& file : replaced)
            if (!fs::remove(file, error) && fs::exists(file)) left.push_back(file);
        replaced = std::move(left);
        if (!replaced.empty()) Sleep(300);
    }
#endif
}
} // namespace lunanx
