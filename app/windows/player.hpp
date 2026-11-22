// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "luna/luna_client.hpp"

#include <windows.h>

namespace lunaone::windows
{
void Play(HWND owner, const lunanx::luna::LunaClient& client, lunanx::luna::Game game);
}
