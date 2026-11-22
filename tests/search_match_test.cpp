#include "luna/search_match.hpp"

#include <cassert>
#include <iostream>

int main()
{
    using namespace lunanx::luna;
    // Exact title, start of title, start of a word, inside a word.
    assert(GameTitleMatchScore("Alan Wake 2", "alan wake 2") == 5);
    assert(GameTitleMatchScore("Alan Wake 2", "ALAN") == 4);
    assert(GameTitleMatchScore("Alan Wake 2", "wake") == 3);
    assert(GameTitleMatchScore("Alan Wake 2", "ake") == 2);
    // Words in any order, missing or extra spaces.
    assert(GameTitleMatchScore("Alan Wake 2", "wake alan") == 1);
    assert(GameTitleMatchScore("Alan Wake 2", "alanwake2") == 1);
    assert(GameTitleMatchScore("Alan Wake 2", "  alan   wake2 ") == 1);
    // A typo is left to Luna's own search; unrelated text never matches.
    assert(GameTitleMatchScore("Alan Wake 2", "alan walk 2") == 0);
    assert(GameTitleMatchScore("Sonic Mania Plus", "zzzz") == 0);
    // Empty or punctuation-only input never matches every game.
    assert(GameTitleMatchScore("Alan Wake 2", "") == 0);
    assert(GameTitleMatchScore("Alan Wake 2", "   ") == 0);
    assert(GameTitleMatchScore("Alan Wake 2", "!?") == 0);
    // Latin accents, ligatures, trademark symbols and typographic punctuation.
    assert(GameTitleMatchScore("Pok\xC3\xA9mon\xE2\x84\xA2 Caf\xC3\xA9 ReMix", "pokemon cafe") == 4);
    assert(GameTitleMatchScore("POK\xC3\x89MON", "pok\xC3\xA9mon") == 5);
    assert(GameTitleMatchScore("STAR WARS\xE2\x84\xA2 Jedi: Survivor\xC2\xAE", "star wars jedi survivor") == 5);
    assert(GameTitleMatchScore("Assassin\xE2\x80\x99s Creed\xC2\xAE Mirage", "assassins creed") == 1);
    assert(GameTitleMatchScore("Assassin's Creed Mirage", "assassin s creed") == 4);
    assert(GameTitleMatchScore("\xC5\x92uvre", "oeuvre") == 5);
    assert(GameTitleMatchScore("Stra\xC3\x9F" "e", "strasse") == 5);
    assert(GameTitleMatchScore("Qui Veut Gagner Des Millions\xC2\xA0?", "millions") == 3);
    // Other scripts are compared as written.
    assert(GameTitleMatchScore("\xD0\xA2\xD0\xB5\xD1\x82\xD1\x80\xD0\xB8\xD1\x81", "\xD0\xA2\xD0\xB5\xD1\x82\xD1\x80\xD0\xB8\xD1\x81") == 5);
    assert(SearchKey("  Alan  Wake\xE2\x80\x94" "2 ") == "alan wake 2");
    // Truncated UTF-8 at the end of the input is kept without reading past the string.
    assert(!SearchKey("Caf\xC3").empty());
    assert(!SearchKey("\xE2\x84").empty());
    std::cout << "Search matching: case, accents, symbols, word order, spacing and empty input passed\n";
}
