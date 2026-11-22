// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "core/playback_preferences.hpp"
#include "input/luna_input_encoder.hpp"
#include "luna/luna_client.hpp"

#include <android/native_window.h>

namespace lunaone::android
{
void UpdateGamepad(lunanx::input::GamepadState state);
void Play(const lunanx::luna::LunaClient& client, lunanx::luna::Game game, ANativeWindow* surface,
          const lunanx::PlaybackPreferences& settings, const std::string& decoder,
          const std::function<bool()>& cancelled,
          const std::function<void(const std::string&)>& status,
          const std::function<void(const std::string&)>& metrics);
}
