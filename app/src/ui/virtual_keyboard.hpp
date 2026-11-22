// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "input/luna_input_encoder.hpp"

#include <cstddef>
#include <string>
#include <vector>

namespace lunanx::ui
{
// L1 + R1 + L2 + R2 held together shows or hides the on-screen keyboard.
constexpr bool VirtualKeyboardChord(const input::GamepadState& pad)
{
    constexpr auto shoulders = input::kLeftShoulder | input::kRightShoulder;
    return (pad.buttons & shoulders) == shoulders && pad.left_trigger >= 128 && pad.right_trigger >= 128;
}

enum class KeyKind { Character, Shift, Layout, Space, Backspace, Submit };

struct VirtualKey
{
    std::string label;
    KeyKind kind = KeyKind::Character;
    // Width in key units: letters are 1, the space bar is wider.
    int span = 1;
};

// On-screen keyboard for a controller, in the layout of the interface
// language: AZERTY in French, QWERTZ in German, ЙЦУКЕН in Russian with a
// Latin layout one key away, QWERTY with the language's letters otherwise.
// Digits on top, the language's letters, then shift, space, delete and OK.
class VirtualKeyboard
{
  public:
    explicit VirtualKeyboard(const std::string& language);

    const std::vector<std::vector<VirtualKey>>& Rows() const;
    std::size_t Row() const { return row_; }
    std::size_t Column() const { return column_; }
    bool Shifted() const { return shifted_; }

    // Moves the focus; left and right wrap within a row, up and down keep
    // the horizontal position as closely as the key widths allow.
    void Move(int dx, int dy);
    void Focus(std::size_t row, std::size_t column);
    // Applies the focused key to the text. Returns true for OK.
    bool Press(std::string& text);
    void ToggleShift();
    static void Backspace(std::string& text);

  private:
    void Build();
    double Centre(std::size_t row, std::size_t column) const;

    std::vector<std::vector<std::string>> lower_, upper_, latin_lower_, latin_upper_;
    std::vector<std::vector<VirtualKey>> rows_;
    std::size_t row_ = 1, column_ = 0;
    bool shifted_ = false;
    bool latin_ = false;
    bool has_latin_ = false;
};
} // namespace lunanx::ui
