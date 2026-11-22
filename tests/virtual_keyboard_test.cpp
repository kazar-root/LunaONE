// SPDX-License-Identifier: GPL-3.0-only

#include "core/i18n.hpp"
#include "ui/virtual_keyboard.hpp"

#include <cassert>
#include <string>

int main()
{
    using namespace lunanx;
    using ui::VirtualKeyboard;

    // The chord needs both shoulders and both triggers.
    input::GamepadState pad{static_cast<std::uint16_t>(input::kLeftShoulder | input::kRightShoulder), 255, 255};
    assert(ui::VirtualKeyboardChord(pad));
    pad.left_trigger = 40;
    assert(!ui::VirtualKeyboardChord(pad));
    assert(!ui::VirtualKeyboardChord({input::kRightShoulder, 255, 255}));

    // French layout is AZERTY with its accented letters.
    VirtualKeyboard french("fr");
    const auto& rows = french.Rows();
    assert(rows.size() == 5 && rows[0].size() == 10 && rows[0][0].label == "1");
    assert(rows[1][0].label == "a" && rows[1][1].label == "z" && rows[2][9].label == "m");
    assert(rows[3][6].label == "é" && rows[3][9].label == "ç");
    assert(rows[4].front().kind == ui::KeyKind::Shift && rows[4].back().kind == ui::KeyKind::Submit);

    // Typing: shift gives one capital, then lower case again.
    std::string text;
    french.Focus(4, 0);
    assert(!french.Press(text) && french.Shifted());
    french.Focus(1, 0);
    french.Press(text);
    french.Press(text);
    assert(text == "Aa" && !french.Shifted());
    french.Focus(3, 6);
    french.Press(text);
    assert(text == "Aaé");
    // Delete removes a whole UTF-8 letter.
    VirtualKeyboard::Backspace(text);
    assert(text == "Aa");
    french.Focus(4, 1);
    french.Press(text);
    assert(text == "Aa ");
    french.Focus(4, 3);
    assert(french.Press(text));

    // Moving: left wraps, down keeps the column as closely as possible.
    french.Focus(1, 0);
    french.Move(-1, 0);
    assert(french.Column() == 9);
    french.Move(0, 1);
    assert(french.Row() == 2 && french.Column() == 9);
    french.Move(0, 10);
    assert(french.Row() == 2);
    french.Focus(3, 4);
    french.Move(0, 1);
    assert(french.Row() == 4 && french.Rows()[4][french.Column()].kind == ui::KeyKind::Space);

    // German is QWERTZ, English QWERTY, Russian has a Latin layout key.
    assert(VirtualKeyboard("de").Rows()[1][5].label == "z");
    assert(VirtualKeyboard("en").Rows()[1][5].label == "y");
    VirtualKeyboard russian("ru");
    assert(russian.Rows()[1][0].label == "й");
    russian.Focus(4, 1);
    assert(russian.Rows()[4][1].kind == ui::KeyKind::Layout);
    russian.Press(text);
    assert(russian.Rows()[1][0].label == "q");
}
