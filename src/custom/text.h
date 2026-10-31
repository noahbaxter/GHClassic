#pragma once

// What every custom song file does with text: paths as UTF-8 whatever the
// system, names and keys lowercased to match as Clone Hero does, and
// song.ini's names in the Latin-1 the game's fonts draw.

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <sstream>
#include <string>
#include <vector>

namespace gh2::custom
{
    inline std::string lower(std::string s)
    {
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
        return s;
    }

    // Its parts between each `by`, empty ones too but for a last.
    inline std::vector<std::string> split(const std::string &s, char by)
    {
        std::vector<std::string> out;
        std::string part;
        std::istringstream in(s);
        while (std::getline(in, part, by))
            out.push_back(part);
        return out;
    }

    inline std::string utf8(const std::filesystem::path &path)
    {
        const std::u8string s = path.u8string();
        return {s.begin(), s.end()};
    }

    inline std::filesystem::path fromUtf8(const std::string &s)
    {
        return std::filesystem::path(std::u8string(s.begin(), s.end()));
    }

    // Clone Hero's rich text (<color=#f00>, <b>) taken out.
    std::string untagged(const std::string &text);
    // That, in Latin-1: UTF-8's letters as the fonts' own, others as the
    // nearest they have.
    std::string plain(const std::string &text);
}
