// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <filesystem>
#include <string>

namespace lunaone::linux_desktop
{
std::filesystem::path LogoPath();

#if defined(__FreeBSD__)
std::string NetworkLabel();
int BatteryPercent();
#endif
}
