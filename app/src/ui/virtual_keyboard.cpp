// SPDX-License-Identifier: GPL-3.0-only

#include "virtual_keyboard.hpp"
#include "core/i18n.hpp"

#include <algorithm>
#include <cmath>
#include <sstream>

namespace lunanx::ui
{
namespace
{
using Layout = std::vector<std::vector<std::string>>;

Layout Split(std::initializer_list<const char*> rows)
{
    Layout layout;
    for (const char* row : rows)
    {
        std::istringstream keys(row);
        std::vector<std::string> line;
        for (std::string key; keys >> key;) line.push_back(key);
        layout.push_back(std::move(line));
    }
    return layout;
}

struct Letters
{
    Layout lower, upper;
};

// Letter rows of each interface language, lower case then upper case.
Letters LettersFor(const std::string& language)
{
    if (language == "fr")
        return {Split({"a z e r t y u i o p", "q s d f g h j k l m", "w x c v b n é è à ç"}),
                Split({"A Z E R T Y U I O P", "Q S D F G H J K L M", "W X C V B N É È À Ç"})};
    if (language == "de")
        return {Split({"q w e r t z u i o p ü", "a s d f g h j k l ö ä", "y x c v b n m ß - ."}),
                Split({"Q W E R T Z U I O P Ü", "A S D F G H J K L Ö Ä", "Y X C V B N M ß : !"})};
    if (language == "es")
        return {Split({"q w e r t y u i o p", "a s d f g h j k l ñ", "z x c v b n m á é í ó ú"}),
                Split({"Q W E R T Y U I O P", "A S D F G H J K L Ñ", "Z X C V B N M Á É Í Ó Ú"})};
    if (language == "it")
        return {Split({"q w e r t y u i o p", "a s d f g h j k l", "z x c v b n m à è é ì ò ù"}),
                Split({"Q W E R T Y U I O P", "A S D F G H J K L", "Z X C V B N M À È É Ì Ò Ù"})};
    if (language == "pt")
        return {Split({"q w e r t y u i o p", "a s d f g h j k l ç", "z x c v b n m ã õ á é í ó"}),
                Split({"Q W E R T Y U I O P", "A S D F G H J K L Ç", "Z X C V B N M Ã Õ Á É Í Ó"})};
    if (language == "pl")
        return {Split({"q w e r t y u i o p ą ę", "a s d f g h j k l ł ś ć", "z x c v b n m ń ó ź ż"}),
                Split({"Q W E R T Y U I O P Ą Ę", "A S D F G H J K L Ł Ś Ć", "Z X C V B N M Ń Ó Ź Ż"})};
    if (language == "sv")
        return {Split({"q w e r t y u i o p å", "a s d f g h j k l ö ä", "z x c v b n m - . :"}),
                Split({"Q W E R T Y U I O P Å", "A S D F G H J K L Ö Ä", "Z X C V B N M ! ? '"})};
    if (language == "ru")
        return {Split({"й ц у к е н г ш щ з х ъ", "ф ы в а п р о л д ж э", "я ч с м и т ь б ю ё"}),
                Split({"Й Ц У К Е Н Г Ш Щ З Х Ъ", "Ф Ы В А П Р О Л Д Ж Э", "Я Ч С М И Т Ь Б Ю Ё"})};
    // English, Dutch and Chinese (pinyin) titles use the plain Latin letters.
    return {Split({"q w e r t y u i o p", "a s d f g h j k l '", "z x c v b n m - . :"}),
            Split({"Q W E R T Y U I O P", "A S D F G H J K L \"", "Z X C V B N M & ! ?"})};
}

// UTF-8 continuation bytes are 10xxxxxx.
bool Continuation(char byte) { return (static_cast<unsigned char>(byte) & 0xc0) == 0x80; }
} // namespace

VirtualKeyboard::VirtualKeyboard(const std::string& language)
{
    auto letters = LettersFor(language);
    lower_ = std::move(letters.lower);
    upper_ = std::move(letters.upper);
    has_latin_ = language == "ru";
    if (has_latin_)
    {
        auto latin = LettersFor("en");
        latin_lower_ = std::move(latin.lower);
        latin_upper_ = std::move(latin.upper);
    }
    Build();
}

void VirtualKeyboard::Build()
{
    rows_.clear();
    std::vector<VirtualKey> digits;
    const char* const numbers[] = {"1", "2", "3", "4", "5", "6", "7", "8", "9", "0"};
    const char* const symbols[] = {"&", "'", "-", ":", ".", "!", "?", "+", "(", ")"};
    for (std::size_t i = 0; i < 10; ++i) digits.push_back({shifted_ ? symbols[i] : numbers[i]});
    rows_.push_back(std::move(digits));
    const auto& letters = latin_ ? (shifted_ ? latin_upper_ : latin_lower_) : (shifted_ ? upper_ : lower_);
    for (const auto& line : letters)
    {
        std::vector<VirtualKey> keys;
        for (const auto& key : line) keys.push_back({key});
        rows_.push_back(std::move(keys));
    }
    std::vector<VirtualKey> actions{{"⇧", KeyKind::Shift, 2}};
    if (has_latin_) actions.push_back({latin_ ? "АБВ" : "ABC", KeyKind::Layout, 2});
    actions.push_back({Tr("Espace"), KeyKind::Space, has_latin_ ? 4 : 5});
    actions.push_back({"⌫", KeyKind::Backspace, 2});
    actions.push_back({Tr("Valider"), KeyKind::Submit, 2});
    rows_.push_back(std::move(actions));
    row_ = std::min(row_, rows_.size() - 1);
    column_ = std::min(column_, rows_[row_].size() - 1);
}

const std::vector<std::vector<VirtualKey>>& VirtualKeyboard::Rows() const { return rows_; }

// Horizontal centre of a key, with every row centred on the widest one.
double VirtualKeyboard::Centre(std::size_t row, std::size_t column) const
{
    const auto width = [&](std::size_t index) {
        int total = 0;
        for (const auto& key : rows_[index]) total += key.span;
        return total;
    };
    int widest = 0;
    for (std::size_t i = 0; i < rows_.size(); ++i) widest = std::max(widest, width(i));
    double position = (widest - width(row)) / 2.0;
    for (std::size_t i = 0; i < column; ++i) position += rows_[row][i].span;
    return position + rows_[row][column].span / 2.0;
}

void VirtualKeyboard::Move(int dx, int dy)
{
    if (dx != 0)
    {
        const auto count = static_cast<long>(rows_[row_].size());
        column_ = static_cast<std::size_t>(((static_cast<long>(column_) + dx) % count + count) % count);
    }
    if (dy != 0)
    {
        const auto target = static_cast<long>(row_) + dy;
        if (target < 0 || target >= static_cast<long>(rows_.size())) return;
        const double centre = Centre(row_, column_);
        row_ = static_cast<std::size_t>(target);
        std::size_t best = 0;
        for (std::size_t i = 1; i < rows_[row_].size(); ++i)
            if (std::abs(Centre(row_, i) - centre) < std::abs(Centre(row_, best) - centre)) best = i;
        column_ = best;
    }
}

void VirtualKeyboard::Focus(std::size_t row, std::size_t column)
{
    if (row < rows_.size() && column < rows_[row].size())
    {
        row_ = row;
        column_ = column;
    }
}

void VirtualKeyboard::ToggleShift()
{
    shifted_ = !shifted_;
    Build();
}

void VirtualKeyboard::Backspace(std::string& text)
{
    while (!text.empty() && Continuation(text.back())) text.pop_back();
    if (!text.empty()) text.pop_back();
}

bool VirtualKeyboard::Press(std::string& text)
{
    const auto key = rows_[row_][column_];
    switch (key.kind)
    {
    case KeyKind::Character:
        text += key.label;
        // A capital starts a word; the keyboard returns to lower case after it.
        if (shifted_ && row_ > 0)
        {
            shifted_ = false;
            Build();
        }
        return false;
    case KeyKind::Shift:
        ToggleShift();
        return false;
    case KeyKind::Layout:
        latin_ = !latin_;
        Build();
        return false;
    case KeyKind::Space:
        text += ' ';
        return false;
    case KeyKind::Backspace:
        Backspace(text);
        return false;
    case KeyKind::Submit:
        return true;
    }
    return false;
}
} // namespace lunanx::ui
