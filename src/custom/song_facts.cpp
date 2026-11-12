#include "custom/song_facts.h"

#include "custom/text.h"
#include "settings/ini.h"
#include "song_facts_table.h"

#include <cctype>
#include <sstream>
#include <unordered_map>

namespace gh2::custom
{
    namespace
    {
        // Latin-1's past ASCII as Unicode's NFKD leaves them in ASCII,
        // lowercased: accented letters as theirs, ligatures and others none.
        const char *folded(unsigned char c)
        {
            static const char *const kHigh[] = {
                " ", "",  "",  "",   "",  "",  "",  "",  " ", "",  "a", "",  "",  "",  "",  " ",  // A0
                "",  "",  "2", "3",  " ", "",  "",  "",  " ", "1", "o", "",  "14", "12", "34", "", // B0
                "a", "a", "a", "a",  "a", "a", "",  "c", "e", "e", "e", "e", "i", "i", "i", "i",  // C0
                "",  "n", "o", "o",  "o", "o", "o", "",  "",  "u", "u", "u", "u", "y", "",  "",   // D0
                "a", "a", "a", "a",  "a", "a", "",  "c", "e", "e", "e", "e", "i", "i", "i", "i",  // E0
                "",  "n", "o", "o",  "o", "o", "o", "",  "",  "u", "u", "u", "u", "y", "",  "y",  // F0
            };
            return c >= 0xA0u ? kHigh[c - 0xA0u] : "";
        }

        bool word(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; }

        struct Table
        {
            std::unordered_map<std::string, SongFacts> songs;
            std::unordered_map<std::string, char> artists;
        };

        const Table &table()
        {
            static const Table t = []
            {
                Table out;
                std::istringstream in(kSongFactsTable);
                for (std::string line; std::getline(in, line);)
                {
                    const std::vector<std::string> f = split(line, '\t');
                    if (f.size() >= 6u && f[0] == "s")
                        out.songs[f[1] + '\t' + f[2]] = {.gender = f[3] == "-" ? '\0' : f[3][0],
                                                         .keys = f[4] == "1",
                                                         .fast = f[5] == "-" ? std::nullopt : std::optional<bool>(f[5] == "fast"),
                                                         .song = true};
                    else if (f.size() >= 3u && f[0] == "a" && !f[2].empty())
                        out.artists[f[1]] = f[2][0];
                }
                return out;
            }();
            return t;
        }
    }

    std::string factKey(const std::string &name)
    {
        std::string s;
        for (const unsigned char c : ini::trim(plain(untagged(name))))
            s += c >= 0x80u ? std::string(folded(c)) : c == '&' ? std::string("and") : std::string(1, char(std::tolower(c)));
        // (...) and [...], each to its first close.
        std::string bare;
        for (size_t i = 0; i < s.size(); ++i)
        {
            const char close = s[i] == '(' ? ')' : s[i] == '[' ? ']' : '\0';
            if (const size_t end = close ? s.find(close, i + 1u) : std::string::npos; end != std::string::npos)
                i = end;
            else
                bare += s[i];
        }
        // Its spaces after "the " go with the rest that isn't a letter or digit.
        if (bare.rfind("the", 0) == 0 && bare.size() > 3u && std::isspace(static_cast<unsigned char>(bare[3])))
            bare.erase(0, 3u);
        for (size_t i = 0; i < bare.size(); ++i)
            if (i == 0u || !word(bare[i - 1u]))
                for (const char *w : {"feat", "featuring", "ft"})
                {
                    const size_t n = std::char_traits<char>::length(w);
                    if (bare.compare(i, n, w) == 0 && (i + n == bare.size() || !word(bare[i + n])))
                    {
                        bare.resize(i);
                        break;
                    }
                }
        std::string out;
        for (const char c : bare)
            if (std::isalnum(static_cast<unsigned char>(c)))
                out += c;
        return out;
    }

    std::optional<SongFacts> songFacts(const std::string &artist, const std::string &title)
    {
        const std::string a = factKey(artist);
        if (a.empty())
            return std::nullopt;
        const Table &t = table();
        if (const auto it = t.songs.find(a + '\t' + factKey(title)); it != t.songs.end())
            return it->second;
        if (const auto it = t.artists.find(a); it != t.artists.end())
            return SongFacts{.gender = it->second};
        return std::nullopt;
    }
}
