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
}
