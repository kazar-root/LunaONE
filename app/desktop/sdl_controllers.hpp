// SPDX-License-Identifier: GPL-3.0-only
// Controllers read through SDL2: Xbox, PlayStation (DualShock 4, DualSense),
// Nintendo (Pro Controller, Joy-Con), 8BitDo and the other pads SDL knows, over
// USB or Bluetooth. 8BitDo pads work in each of their modes: X-input like an
// Xbox pad, Switch like a Pro Controller, D-input through SDL's mappings.
#pragma once

#include "input/controller_labels.hpp"
#include "input/luna_input_encoder.hpp"

#include <SDL.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <utility>

namespace lunaone::desktop
{
// Before SDL_Init(SDL_INIT_GAMECONTROLLER). A, B, X, Y by position, as Luna's
// Xbox layout and the button names expect: the bottom button is A, even where
// a Nintendo or 8BitDo pad prints B on it.
inline void SetControllerHints()
{
    SDL_SetHint(SDL_HINT_GAMECONTROLLER_USE_BUTTON_LABELS, "0");
}

// The labels printed on the pad, for hints and settings.
inline lunanx::input::ControllerFamily ControllerFamilyOf(SDL_GameController* controller)
{
    using lunanx::input::ControllerFamily;
    if (!controller) return ControllerFamily::Xbox;
    switch (SDL_GameControllerGetType(controller))
    {
    case SDL_CONTROLLER_TYPE_PS3:
    case SDL_CONTROLLER_TYPE_PS4:
    case SDL_CONTROLLER_TYPE_PS5: return ControllerFamily::PlayStation;
    case SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_PRO:
    case SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_JOYCON_LEFT:
    case SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT:
    case SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_JOYCON_PAIR: return ControllerFamily::Nintendo;
    default: break;
    }
    // SDL's mappings for pads that print B A Y X (8BitDo SN30, Pro 2, M30,
    // Zero…) depend on the button label hint. The Ultimate pads print A B X Y.
    if (char* mapping = SDL_GameControllerMapping(controller))
    {
        const bool nintendo_labels = std::strstr(mapping, "SDL_GAMECONTROLLER_USE_BUTTON_LABELS") != nullptr;
        SDL_free(mapping);
        if (nintendo_labels) return ControllerFamily::Nintendo;
    }
    return lunanx::input::ControllerFamilyFromVendor(SDL_GameControllerGetVendor(controller));
}

// The state of a controller in Luna's (XInput's) format.
inline lunanx::input::GamepadState ReadControllerState(SDL_GameController* controller)
{
    using namespace lunanx::input;
    GamepadState state;
    constexpr std::pair<SDL_GameControllerButton, std::uint16_t> buttons[] = {
        {SDL_CONTROLLER_BUTTON_A, kA}, {SDL_CONTROLLER_BUTTON_B, kB}, {SDL_CONTROLLER_BUTTON_X, kX},
        {SDL_CONTROLLER_BUTTON_Y, kY}, {SDL_CONTROLLER_BUTTON_DPAD_UP, kDpadUp},
        {SDL_CONTROLLER_BUTTON_DPAD_DOWN, kDpadDown}, {SDL_CONTROLLER_BUTTON_DPAD_LEFT, kDpadLeft},
        {SDL_CONTROLLER_BUTTON_DPAD_RIGHT, kDpadRight}, {SDL_CONTROLLER_BUTTON_LEFTSHOULDER, kLeftShoulder},
        {SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, kRightShoulder}, {SDL_CONTROLLER_BUTTON_LEFTSTICK, kLeftThumb},
        {SDL_CONTROLLER_BUTTON_RIGHTSTICK, kRightThumb}, {SDL_CONTROLLER_BUTTON_START, kStart},
        {SDL_CONTROLLER_BUTTON_BACK, kBack}};
    for (const auto& [button, mask] : buttons)
        if (SDL_GameControllerGetButton(controller, button)) state.buttons |= mask;
    const auto axis = [controller](SDL_GameControllerAxis id) { return SDL_GameControllerGetAxis(controller, id); };
    // SDL's sticks point down for a positive Y, Luna's up.
    const auto up = [](std::int16_t value) { return static_cast<std::int16_t>(-std::max<int>(value, -32767)); };
    const auto trigger = [](std::int16_t value) {
        return static_cast<std::uint8_t>(std::max<int>(0, value) * 255 / 32767);
    };
    state.left_x = axis(SDL_CONTROLLER_AXIS_LEFTX);
    state.left_y = up(axis(SDL_CONTROLLER_AXIS_LEFTY));
    state.right_x = axis(SDL_CONTROLLER_AXIS_RIGHTX);
    state.right_y = up(axis(SDL_CONTROLLER_AXIS_RIGHTY));
    state.left_trigger = trigger(axis(SDL_CONTROLLER_AXIS_TRIGGERLEFT));
    state.right_trigger = trigger(axis(SDL_CONTROLLER_AXIS_TRIGGERRIGHT));
    return state;
}

// SDL's game controller subsystem and the pad in use. Create, read and destroy
// it on one thread: SDL's Windows drivers tie their devices to the thread that
// opened the subsystem.
class SdlControllers
{
  public:
    SdlControllers()
    {
        SetControllerHints();
        // The window reads the pad only while it is in front. SDL has no
        // window of its own to watch.
        SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
        SDL_SetMainReady();
        ready_ = SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER) == 0;
        if (ready_)
        {
            // Read() asks for the state: no event queue to fill.
            SDL_JoystickEventState(SDL_IGNORE);
            SDL_GameControllerEventState(SDL_IGNORE);
        }
    }

    ~SdlControllers()
    {
        if (controller_) SDL_GameControllerClose(controller_);
        if (ready_) SDL_QuitSubSystem(SDL_INIT_GAMECONTROLLER);
    }

    SdlControllers(const SdlControllers&) = delete;
    SdlControllers& operator=(const SdlControllers&) = delete;

    // Updates the pads and reads the one in use, or the first connected.
    bool Read(lunanx::input::GamepadState& state)
    {
        if (!ready_) return false;
        SDL_GameControllerUpdate();
        if (controller_ && !SDL_GameControllerGetAttached(controller_))
        {
            SDL_GameControllerClose(controller_);
            controller_ = nullptr;
        }
        for (int i = 0; i < SDL_NumJoysticks() && !controller_; ++i)
            if (SDL_IsGameController(i) && (controller_ = SDL_GameControllerOpen(i)))
                family_ = ControllerFamilyOf(controller_);
        if (!controller_) return false;
        state = ReadControllerState(controller_);
        return true;
    }

    // The labels printed on the pad read last.
    lunanx::input::ControllerFamily Family() const { return family_; }

  private:
    bool ready_ = false;
    SDL_GameController* controller_ = nullptr;
    lunanx::input::ControllerFamily family_ = lunanx::input::ControllerFamily::Xbox;
};
} // namespace lunaone::desktop
