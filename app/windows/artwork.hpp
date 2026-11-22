// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <windows.h>
#include <string>
namespace lunaone::windows
{
HBITMAP LoadArtwork(const std::string& url, int width, int height);
}
