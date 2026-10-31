#include "custom/text.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>

namespace gh2::custom
{
    namespace
    {
        // A letter and the combining accent after it (U+0300 on, as macOS
        // writes file names) as Latin-1's one letter: each accent's bases
        // and what they make.
        struct Accent
        {
            uint32_t mark;
            const char *bases;
            const char *made;
        };
        constexpr Accent kAccents[] = {
            {0x300, "AEIOUaeiou", "\xC0\xC8\xCC\xD2\xD9\xE0\xE8\xEC\xF2\xF9"},
            {0x301, "AEIOUYaeiouy", "\xC1\xC9\xCD\xD3\xDA\xDD\xE1\xE9\xED\xF3\xFA\xFD"},
            {0x302, "AEIOUaeiou", "\xC2\xCA\xCE\xD4\xDB\xE2\xEA\xEE\xF4\xFB"},
            {0x303, "ANOano", "\xC3\xD1\xD5\xE3\xF1\xF5"},
            {0x308, "AEIOUaeiouy", "\xC4\xCB\xCF\xD6\xDC\xE4\xEB\xEF\xF6\xFC\xFF"},
            {0x30A, "Aa", "\xC5\xE5"},
            {0x327, "Cc", "\xC7\xE7"},
        };

        // Latin Extended-A (U+0100 on), each letter's own without its
        // accent; # is two letters (IJ ij OE oe).
        constexpr const char kLatinA[] = "AaAaAaCcCcCcCcDdDdEeEeEeEeEeGgGgGgGgHhHhIiIiIiIiIi##JjKkkLlLlLlL"
                                         "lLlNnNnNnnNnOoOoOo##RrRrRrSsSsSsSsTtTtTtUuUuUuUuUuUuWwYyYZzZzZzs";

        // Cyrillic's А to я (U+0410 on), spelled out and as the Latin
        // letters they look like; the second for a name that is mostly
        // Latin already, styled with them (ЯOCK).
        constexpr const char *kCyrillic[64] = {
            "A", "B", "V", "G", "D", "E", "Zh", "Z", "I", "Y", "K", "L", "M", "N", "O", "P",
            "R", "S", "T", "U", "F", "Kh", "Ts", "Ch", "Sh", "Shch", "", "Y", "", "E", "Yu", "Ya",
            "a", "b", "v", "g", "d", "e", "zh", "z", "i", "y", "k", "l", "m", "n", "o", "p",
            "r", "s", "t", "u", "f", "kh", "ts", "ch", "sh", "shch", "", "y", "", "e", "yu", "ya",
        };
        constexpr const char *kCyrillicLooks[64] = {
            "A", "B", "B", "r", "A", "E", "X", "3", "N", "N", "K", "JI", "M", "H", "O", "N",
            "P", "C", "T", "Y", "O", "X", "U", "4", "W", "W", "b", "bI", "b", "E", "IO", "R",
            "a", "6", "B", "r", "A", "e", "x", "3", "u", "u", "k", "n", "m", "H", "o", "n",
            "p", "c", "T", "y", "o", "x", "u", "4", "w", "w", "b", "bl", "b", "e", "io", "R",
        };
        // Greek's Α to ω (U+0391 on, 0x3A2 and 0x3AA-0x3B0 unused), the same two ways.
        constexpr const char *kGreek[57] = {
            "A", "V", "G", "D", "E", "Z", "I", "Th", "I", "K", "L", "M", "N", "X", "O", "P", "R", "", "S", "T",
            "Y", "F", "Ch", "Ps", "O", "", "", "", "", "", "", "", "a", "v", "g", "d", "e", "z", "i", "th",
            "i", "k", "l", "m", "n", "x", "o", "p", "r", "s", "s", "t", "y", "f", "ch", "ps", "o",
        };
        constexpr const char *kGreekLooks[57] = {
            "A", "B", "r", "A", "E", "Z", "H", "O", "I", "K", "A", "M", "N", "E", "O", "N", "P", "", "E", "T",
            "Y", "O", "X", "Y", "O", "", "", "", "", "", "", "", "a", "B", "y", "d", "e", "z", "n", "o",
            "i", "k", "A", "u", "v", "e", "o", "n", "p", "c", "o", "t", "u", "o", "x", "w", "w",
        };
        // Greek's accented vowels (U+0386 to U+038F, U+03AC to U+03CE) as their own.
        uint32_t unaccentedGreek(uint32_t c)
        {
            switch (c)
            {
            case 0x386: return 0x391;
            case 0x388: return 0x395;
            case 0x389: return 0x397;
            case 0x38A: return 0x399;
            case 0x38C: return 0x39F;
            case 0x38E: return 0x3A5;
            case 0x38F: return 0x3A9;
            case 0x3AC: return 0x3B1;
            case 0x3AD: return 0x3B5;
            case 0x3AE: return 0x3B7;
            case 0x3AF: return 0x3B9;
            case 0x3CC: return 0x3BF;
            case 0x3CD: return 0x3C5;
            case 0x3CE: return 0x3C9;
            default: return c;
            }
        }

        // Past Latin-1, what reads the same in it: a letter without its
        // accent, Cyrillic and Greek in Latin letters (as they look if
        // `looks`), a symbol with a stand-in as that and any other let go,
        // and "?" for the rest (Japanese, Chinese and the like).
        std::string nearest(uint32_t c, bool looks)
        {
            switch (c)
            {
            case 0x2018: case 0x2019: case 0x201A: case 0x2032: return "'";
            case 0x201C: case 0x201D: case 0x201E: case 0x2033: return "\"";
            case 0x2010: case 0x2011: case 0x2012: case 0x2013: case 0x2014: case 0x2015: case 0x2212: return "-";
            case 0x2026: return "...";
            case 0x2022: case 0x2605: case 0x2606: case 0x2731: return "*";
            case 0x0178: return "Y";
            case 0x0218: return "S";
            case 0x0219: return "s";
            case 0x021A: return "T";
            case 0x021B: return "t";
            case 0x01A0: return "O";
            case 0x01A1: return "o";
            case 0x01AF: return "U";
            case 0x01B0: return "u";
            case 0x0401: return looks ? "E" : "Yo";
            case 0x0451: return looks ? "e" : "yo";
            case 0x0404: return looks ? "E" : "Ye";
            case 0x0454: return looks ? "e" : "ye";
            case 0x0406: return "I";
            case 0x0456: return "i";
            case 0x0407: return looks ? "I" : "Yi";
            case 0x0457: return looks ? "i" : "yi";
            case 0x0490: return looks ? "r" : "G";
            case 0x0491: return looks ? "r" : "g";
            case 0x3000: return " ";
            default: break;
            }
            if (c >= 0x100 && c < 0x180)
            {
                const char k = kLatinA[c - 0x100];
                return k != '#' ? std::string(1, k)
                       : c == 0x132 ? "IJ" : c == 0x133 ? "ij" : c == 0x152 ? "OE" : "oe";
            }
            if (c >= 0x410 && c < 0x450)
                return (looks ? kCyrillicLooks : kCyrillic)[c - 0x410];
            if (const uint32_t g = unaccentedGreek(c); g >= 0x391 && g <= 0x3C9)
                return (looks ? kGreekLooks : kGreek)[g - 0x391];
            // Full-width forms, as Japanese names write Latin letters.
            if (c >= 0xFF01 && c <= 0xFF5E)
                return std::string(1, static_cast<char>(c - 0xFEE0));
            // Symbols, arrows, shapes, dingbats, emoji and the variation
            // selectors after them.
            if ((c >= 0x2000 && c < 0x2C00) || (c >= 0x1F000 && c < 0x1FB00) || (c >= 0xFE00 && c < 0xFE10))
                return "";
            return "?";
        }

        // Latin-1's symbols from ¢ to ¾, which no disc's fonts draw but
        // © and ®, as above.
        constexpr const char *kLatin1Symbols[29] = {
            "c", "L", "", "Y", "|", "S", "", "\xA9", "a", "\"", "-", "", "\xAE", "", "", "+-",
            "2", "3", "'", "u", "", "*", "", "1", "o", "\"", "1/4", "1/2", "3/4",
        };

        // Windows-1252's own 0x80-0x9F, what older song.inis hold, as above.
        constexpr uint16_t kCp1252[32] = {
            0x20AC, 0, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0, 0x017D, 0,
            0, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0, 0x017E, 0x0178,
        };

        // UTF-8 as the game's fonts draw text, a byte a letter in Latin-1.
        // A byte that starts no UTF-8 letter is taken as Windows-1252's.
        std::string latin1(const std::string &text)
        {
            const bool looks = std::any_of(text.begin(), text.end(), [](unsigned char c) { return c < 0x80 && std::isalpha(c); });
            std::string out;
            for (size_t i = 0; i < text.size();)
            {
                const auto byte = [&](size_t k) { return static_cast<unsigned char>(text[k]); };
                const unsigned char b = byte(i);
                size_t n = b >= 0xF0 && b < 0xF5 ? 4 : b >= 0xE0 ? 3 : b >= 0xC2 && b < 0xE0 ? 2 : 1;
                if (b >= 0xF5)
                    n = 1;
                uint32_t c = n == 1 ? b : b & (0xFFu >> (n + 1));
                for (size_t k = 1; k < n; ++k)
                    if (i + k >= text.size() || (byte(i + k) & 0xC0) != 0x80)
                        n = 1, c = b;
                    else
                        c = c << 6 | (byte(i + k) & 0x3F);
                i += n;
                if (n == 1 && b >= 0x80 && b < 0xA0)
                    c = kCp1252[b - 0x80] ? kCp1252[b - 0x80] : '?';
                if (c >= 0x300 && c < 0x370)
                {
                    // Onto the letter before it, or let go.
                    for (const Accent &a : kAccents)
                        if (const char *at = a.mark == c && !out.empty() ? std::strchr(a.bases, out.back()) : nullptr)
                            out.back() = a.made[at - a.bases];
                    continue;
                }
                if (c == 0xA0)
                    out += ' ';
                else if (c == 0xAD || c == 0xFEFF || (c >= 0x200B && c <= 0x200F))
                    continue;
                else if (c >= 0xA2 && c <= 0xBE)
                    out += kLatin1Symbols[c - 0xA2];
                else if (c == 0xD7 || c == 0xF7)
                    out += c == 0xD7 ? 'x' : '/';
                else
                    out += c < 0x100 ? std::string(1, static_cast<char>(c)) : nearest(c, looks);
            }
            return out;
        }
    }

    std::string untagged(const std::string &text)
    {
        std::string out;
        bool tag = false;
        for (const char c : text)
        {
            if (c == '<')
                tag = true;
            else if (c == '>' && tag)
                tag = false;
            else if (!tag)
                out += c;
        }
        return out;
    }

    std::string plain(const std::string &text) { return latin1(untagged(text)); }
}
