// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "luna/luna_client.hpp"

#include <chrono>
#include <functional>

namespace lunaone::apple
{

void Play(const lunanx::luna::LunaClient& client, lunanx::luna::Game game,
          std::chrono::seconds duration, std::function<bool()> cancelled, bool modal = false);

} // namespace lunaone::apple
