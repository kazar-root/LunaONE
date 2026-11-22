// LunaNX-Origin: KazaR-RooT | urn:uuid:57e9877e-564c-4275-ae2a-a70f276bfbed | unit=3cdb8f708bed938ea578
// Project origin: https://github.com/kazar-root/LunaNX (third-party notices remain applicable).

#pragma once

#include <algorithm>
#include <cctype>
#include <string>

namespace lunanx::luna
{
namespace search_detail
{

// Lettre de base d'un caractère latin accentué codé « C3 xx » en UTF-8 ; vide pour × et ÷.
inline std::string FoldLatin1(unsigned char second)
{
    if (second == 0x9F)
        return "ss";
    if (second == 0xBF)
        return "y";
    if (second == 0x97 || second == 0xB7)
        return {};
    if (second >= 0xA0)
        second = static_cast<unsigned char>(second - 0x20);
    if (second <= 0x85)
        return "a";
    switch (second)
    {
        case 0x86: return "ae";
        case 0x87: return "c";
        case 0x90: return "d";
        case 0x91: return "n";
        case 0x98: return "o";
        case 0x9D: return "y";
        case 0x9E: return "th";
        default: break;
    }
    if (second <= 0x8B)
        return "e";
    if (second <= 0x8F)
        return "i";
    if (second <= 0x96)
        return "o";
    return "u";
}

inline std::string WithoutSpaces(std::string text)
{
    text.erase(std::remove(text.begin(), text.end(), ' '), text.end());
    return text;
}

} // namespace search_detail

// Texte comparable pour la recherche : minuscules sans accents latins, mots séparés par une
// seule espace. Les autres écritures (cyrillique, japonais…) sont gardées telles quelles.
inline std::string SearchKey(const std::string& text)
{
    std::string out;
    bool space = false;
    auto append = [&](const std::string& letters) {
        if (letters.empty())
        {
            space = true;
            return;
        }
        if (space && !out.empty())
            out.push_back(' ');
        space = false;
        out += letters;
    };
    for (size_t i = 0; i < text.size(); ++i)
    {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        const unsigned char next = i + 1 < text.size() ? static_cast<unsigned char>(text[i + 1]) : 0;
        if (c < 0x80)
        {
            append(std::isalnum(c) ? std::string(1, static_cast<char>(std::tolower(c))) : std::string());
            continue;
        }
        if (c == 0xE2 && i + 2 < text.size())
        {
            // ™ ne coupe pas le mot ; tirets, guillemets et autres signes typographiques séparent.
            if (next == 0x84 && static_cast<unsigned char>(text[i + 2]) == 0xA2)
            {
                i += 2;
                continue;
            }
            if (next == 0x80 || next == 0x81)
            {
                i += 2;
                append({});
                continue;
            }
        }
        if (c == 0xC2 && next >= 0x80 && next <= 0xBF)
        {
            ++i;
            // ® et © ne coupent pas le mot ; espace insécable, « », ¡, ¿… séparent.
            if (next != 0xAE && next != 0xA9)
                append({});
            continue;
        }
        if (c == 0xC3 && next >= 0x80 && next <= 0xBF)
        {
            ++i;
            append(search_detail::FoldLatin1(next));
            continue;
        }
        if (c == 0xC5 && (next == 0x92 || next == 0x93))
        {
            ++i;
            append("oe");
            continue;
        }
        append(std::string(1, static_cast<char>(c)));
    }
    return out;
}

// Recherche locale : 0 si le titre ne correspond pas à la saisie, sinon un score (plus il est
// élevé, plus la correspondance est forte). Casse, accents latins, ponctuation, espaces et
// symboles ™ ® © sont ignorés ; les mots de la saisie peuvent être dans n'importe quel ordre.
inline int GameTitleMatchScore(const std::string& title, const std::string& query)
{
    const std::string needle = SearchKey(query);
    const std::string haystack = SearchKey(title);
    if (needle.empty() || haystack.empty())
        return 0;
    if (haystack == needle)
        return 5;
    if (haystack.rfind(needle, 0) == 0)
        return 4;
    const size_t found = haystack.find(needle);
    if (found != std::string::npos)
        return haystack[found - 1] == ' ' ? 3 : 2;
    // Mots dans le désordre (« wake alan ») : chacun doit figurer dans le titre.
    bool all_words = true;
    size_t start = 0;
    while (start < needle.size() && all_words)
    {
        const size_t end = std::min(needle.find(' ', start), needle.size());
        all_words = haystack.find(needle.substr(start, end - start)) != std::string::npos;
        start = end + 1;
    }
    if (all_words)
        return 1;
    // Espaces oubliées ou en trop (« alanwake 2 », « alan wake2 »).
    return search_detail::WithoutSpaces(haystack).find(search_detail::WithoutSpaces(needle)) != std::string::npos
        ? 1 : 0;
}

} // namespace lunanx::luna
