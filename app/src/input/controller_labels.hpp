// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <string>
#include <string_view>

namespace lunanx::input
{
// The labels printed on a controller. Luna and the button masks use the Xbox
// layout; hints, settings and launch screens name the buttons as the
// controller in hand shows them.
enum class ControllerFamily { Xbox, PlayStation, Nintendo };
enum class PadButton { A, B, X, Y, LeftShoulder, RightShoulder, LeftTrigger, RightTrigger, LeftStick, RightStick, Menu };

// From a product name ("DualSense Wireless Controller", "Pro Controller"…)
// or a USB vendor id (Sony 054c, Nintendo 057e); Xbox otherwise.
ControllerFamily ControllerFamilyFromName(std::string_view name);
ControllerFamily ControllerFamilyFromVendor(unsigned vendor_id);

// A, B, X, Y are positions: A the bottom button, B the right one, X the left
// one, Y the top one. ✕ ○ □ △ on PlayStation, B A Y X on Nintendo.
std::string PadButtonName(ControllerFamily family, PadButton button);
// Leave a game: L3 + R3 + R1, LS + RS + RB…
std::string ExitChordName(ControllerFamily family);
// On-screen keyboard: L1 + L2 + R1 + R2, LB + LT + RB + RT, L + ZL + R + ZR.
std::string KeyboardChordName(ControllerFamily family);
// Technical overlay: L3 + R3, LS + RS.
std::string OverlayChordName(ControllerFamily family);
} // namespace lunanx::input
