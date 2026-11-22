// SPDX-License-Identifier: GPL-3.0-only

#include "controller_labels.hpp"

#include <algorithm>
#include <array>
#include <cctype>

namespace lunanx::input
{
namespace
{
bool Contains(std::string_view text, std::string_view word)
{
    return std::search(text.begin(), text.end(), word.begin(), word.end(), [](char a, char b) {
        return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
    }) != text.end();
}
} // namespace

ControllerFamily ControllerFamilyFromName(std::string_view name)
{
    for (const auto* word : {"dualshock", "dualsense", "playstation", "ps4", "ps5", "sony"})
        if (Contains(name, word)) return ControllerFamily::PlayStation;
    for (const auto* word : {"nintendo", "switch", "joy-con", "joycon", "pro controller"})
        if (Contains(name, word)) return ControllerFamily::Nintendo;
    return ControllerFamily::Xbox;
}

ControllerFamily ControllerFamilyFromVendor(unsigned vendor_id)
{
    if (vendor_id == 0x054c) return ControllerFamily::PlayStation;
    if (vendor_id == 0x057e) return ControllerFamily::Nintendo;
    return ControllerFamily::Xbox;
}

std::string PadButtonName(ControllerFamily family, PadButton button)
{
    // A B X Y, shoulders, triggers, stick buttons, menu.
    static constexpr std::array<const char*, 11> xbox{"A", "B", "X", "Y", "LB", "RB", "LT", "RT", "LS", "RS", "☰"};
    static constexpr std::array<const char*, 11> playstation{"✕", "○", "□", "△", "L1", "R1", "L2", "R2", "L3", "R3",
                                                             "Options"};
    static constexpr std::array<const char*, 11> nintendo{"B", "A", "Y", "X", "L", "R", "ZL", "ZR", "LS", "RS", "+"};
    const auto& names = family == ControllerFamily::PlayStation ? playstation
                      : family == ControllerFamily::Nintendo    ? nintendo
                                                                : xbox;
    return names[static_cast<std::size_t>(button)];
}

std::string ExitChordName(ControllerFamily family)
{
    return PadButtonName(family, PadButton::LeftStick) + " + " + PadButtonName(family, PadButton::RightStick) + " + " +
           PadButtonName(family, PadButton::RightShoulder);
}

std::string KeyboardChordName(ControllerFamily family)
{
    return PadButtonName(family, PadButton::LeftShoulder) + " + " + PadButtonName(family, PadButton::LeftTrigger) + " + " +
           PadButtonName(family, PadButton::RightShoulder) + " + " + PadButtonName(family, PadButton::RightTrigger);
}

std::string OverlayChordName(ControllerFamily family)
{
    return PadButtonName(family, PadButton::LeftStick) + " + " + PadButtonName(family, PadButton::RightStick);
}
} // namespace lunanx::input
