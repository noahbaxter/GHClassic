// The index is its version's line, then a line a song, its fields split by
// tabs: folder, chart, the two write times, why it does not play and its
// kind, parts, stems and sections each split by '|', length, the charted
// difficulties, the chart's MD5, album art, Song::chartHas, band BPM, then
// its tags as key=value, each a field. A '|' in a section's name splits it.
// A tab or line break in a value is a space.

#include "custom/index.h"

#include "custom/catalog.h"
#include "custom/text.h"
#include "settings/ini.h"

#include <algorithm>
#include <fstream>
#include <map>

namespace gh2::custom
{
    namespace
    {
        namespace fs = std::filesystem;

        // The index's first line, changed with its fields.
        constexpr const char *kIndexVersion = "# GH Classic songs index 9";

        std::string flat(std::string s)
        {
            std::replace_if(s.begin(), s.end(), [](char c) { return c == '\t' || c == '\n' || c == '\r'; }, ' ');
            return s;
        }
    }

    std::vector<Song> readIndex(const fs::path &file)
    {
        std::vector<Song> songs;
        std::ifstream in(file);
        // One of another layout is none: every song is read again.
        std::string version;
        if (!std::getline(in, version) || version != kIndexVersion)
            return songs;
        for (std::string line; std::getline(in, line);)
        {
            std::vector<std::string> f = split(line, '\t');
            // split leaves out a last field that is empty (no art, no tags).
            if (f.size() < 12u)
                continue;
            f.resize(std::max<size_t>(f.size(), 15u));
            Song song;
            song.dir = fromUtf8(f[0]);
            song.chart = f[1];
            song.iniTime = std::strtoll(f[2].c_str(), nullptr, 10);
            song.chartTime = std::strtoll(f[3].c_str(), nullptr, 10);
            song.why = f[4];
            song.reject = static_cast<Reject>(std::strtoul(f[5].c_str(), nullptr, 10));
            song.parts = static_cast<uint8_t>(std::strtoul(f[6].c_str(), nullptr, 10));
            song.stems = split(f[7], '|');
            song.sections = split(f[8], '|');
            song.length = std::strtod(f[9].c_str(), nullptr);
            song.charted = static_cast<uint16_t>(std::strtoul(f[10].c_str(), nullptr, 10));
            song.chartMd5 = f[11];
            song.art = f[12];
            song.chartHas = static_cast<uint8_t>(std::strtoul(f[13].c_str(), nullptr, 10));
            song.bandBpm = std::strtof(f[14].c_str(), nullptr);
            for (size_t i = 15u; i < f.size(); ++i)
                if (const size_t eq = f[i].find('='); eq != std::string::npos)
                    song.tags[f[i].substr(0, eq)] = f[i].substr(eq + 1u);
            songs.push_back(std::move(song));
        }
        return songs;
    }

    void writeIndex(const fs::path &file, const std::vector<Song> &songs)
    {
        ini::writeReplacing(utf8(file), [&](std::ofstream &out)
        {
            out << kIndexVersion << '\n';
            for (const Song &s : songs)
            {
                const auto joined = [](const std::vector<std::string> &list)
                {
                    std::string s;
                    for (const std::string &item : list)
                        s += (s.empty() ? "" : "|") + item;
                    return flat(s);
                };
                out << flat(utf8(s.dir)) << '\t' << s.chart << '\t' << s.iniTime << '\t' << s.chartTime << '\t'
                    << flat(s.why) << '\t' << int(s.reject) << '\t' << int(s.parts) << '\t' << joined(s.stems) << '\t' << joined(s.sections)
                    << '\t' << s.length << '\t' << s.charted << '\t' << s.chartMd5 << '\t'
                    << flat(s.art) << '\t' << int(s.chartHas) << '\t' << s.bandBpm;
                for (const auto &[key, value] : s.tags)
                    out << '\t' << flat(key) << '=' << flat(value);
                out << '\n';
            }
        });
    }

    void writeBadSongs(const fs::path &file, const std::vector<Song> &songs)
    {
        // Each kind's heading, by Reject, and whether its songs' own why is
        // more than it says.
        static const std::pair<const char *, bool> kKinds[] = {
            {"", false},
            {"These folders have no chart (notes.mid or notes.chart).", false},
            {"These charts are corrupted and can't be read.", true},
            {"These charts have no part GH Classic plays (guitar, co-op guitar, rhythm or bass).", false},
            {"These songs have no audio (.ogg, .opus, .mp3, .wav or .aiff).", false},
            {"These songs' audio is corrupted or in a format GH Classic can't play.", true},
            {"These .sng packages can't be read.", true},
        };
        std::ofstream out(file, std::ios::trunc);
        out << "# Songs in your chart folders that GH Classic cannot play, and why. Rewritten by each scan.\n";
        for (size_t kind = 1u; kind < std::size(kKinds); ++kind)
        {
            bool any = false;
            for (const Song &s : songs)
                if (s.reject == static_cast<Reject>(kind))
                {
                    if (!any)
                        out << '\n' << kKinds[kind].first << '\n';
                    any = true;
                    out << utf8(s.dir) << '\n';
                    if (kKinds[kind].second)
                        out << "    " << s.why << '\n';
                }
        }
    }

    void writeSongList(const fs::path &file, const std::vector<Song> &songs, const std::map<fs::path, fs::path> &copied)
    {
        // A field quoted where it holds a comma, a quote or a line break.
        const auto field = [](const std::string &s)
        {
            if (s.find_first_of(",\"\r\n") == std::string::npos)
                return s;
            std::string out = "\"";
            for (const char c : s)
                out += c == '"' ? std::string("\"\"") : std::string(1, c);
            return out + "\"";
        };
        const auto text = [&](const Song &s, const char *key) { return field(untagged(tag(s.tags, key).value_or(""))); };
        std::ofstream out(file, std::ios::trunc | std::ios::binary);
        // UTF-8's mark first, for a spreadsheet to read the names as such.
        out << "\xEF\xBB\xBF" << "name,artist,album,year,genre,charter,length,parts,folder\r\n";
        for (const Song &s : songs)
        {
            if (!s.why.empty() || copied.count(s.dir))
                continue;
            std::string parts;
            for (int p = 0; p < 4; ++p)
            {
                const unsigned charted = (s.charted >> (4 * p)) & 0xfu;
                if (!charted)
                    continue;
                parts += (parts.empty() ? "" : "; ") + std::string(kPartNames[p]) + " ";
                for (int d = 0; d < 4; ++d)
                    if (charted & (1u << d))
                        parts += "EMHX"[d];
            }
            out << field(title(s)) << ',' << text(s, "artist") << ',' << text(s, "album") << ',' << text(s, "year") << ','
                << text(s, "genre") << ',' << text(s, "charter") << ',' << lengthText(s.length) << ',' << field(parts) << ','
                << field(utf8(s.dir)) << "\r\n";
        }
    }
}
