// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <string>
#include <string_view>

namespace lunanx
{
inline constexpr char ProjectCredit[] = "KazaR-RooT";
inline constexpr char ReleasesUrl[] = "https://github.com/kazar-root/LunaONE/releases";

std::string_view InstalledVersion();
// The version as shown to people: « V0.1 », the patch number only when it is not 0.
std::string VersionLabel();
// About: LunaONE is not affiliated with Amazon or Amazon Luna.
inline constexpr char Disclaimer[] = "Projet non affilié à Amazon ni à Amazon Luna.";
std::string CheckForUpdates(std::string_view installed = InstalledVersion());

// The newest release on GitHub and, when LunaONE can install it itself, this
// device's file in it.
struct UpdateInfo
{
    // What CheckForUpdates shows.
    std::string message;
    // GitHub has a newer version.
    bool available = false;
    // "0.4", or "0.4.1".
    std::string version;
    // "LunaONE-V0.4-macOS-universal.zip", its address and size, its SHA-256
    // as GitHub computed it, and the release's SHA256SUMS.txt.
    std::string asset_name, asset_url, sha256, sums_url;
    long long asset_size = 0;

    bool Downloadable() const { return available && !asset_url.empty() && (!sha256.empty() || !sums_url.empty()); }
};
UpdateInfo FindUpdate(std::string_view installed = InstalledVersion());
// The end of this build's file name in a release ("-macOS-universal.zip"), or
// empty where LunaONE cannot install a release itself: iPhone and iPad, whose
// app must be signed again, and Linux builds made against glibc, since the
// Linux release is built on Alpine (musl).
std::string_view UpdateAssetSuffix();
// Downloads the update's file and checks its size and its SHA-256, the one
// GitHub gives or else the release's SHA256SUMS.txt. Throws with a translated
// message.
std::string DownloadUpdate(const UpdateInfo& update);
}
