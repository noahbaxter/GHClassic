// notes.mid is Rock Band's form as Clone Hero reads it: each difficulty's
// lanes from note 60, 72, 84 and 96 (easy to expert); 116 star power, 103 a
// solo (or the star power, where nothing is on 116).
//
// notes.chart is text: [Song], [SyncTrack] (B a tempo in thousandths of a
// beat a minute, TS a signature), [Events], and a section a part and
// difficulty, [ExpertSingle], each line "<tick> = N <lane> <length>": 0
// to 4 the frets; "S 2 <length>" star power, "E solo" and "E soloend".
//
// Opens (a .chart's 7, Phase Shift's over the green note), taps and forced
// HOPOs and strums are left out: GH2 plays none.

#include "custom/chart.h"

#include "custom/text.h"
#include "formats/midi.h"
#include "settings/ini.h"

#include <algorithm>
#include <sstream>

namespace gh2::custom
{
    namespace
    {
        // Gems in tick order, those with no lane left out.
        std::vector<Gem> gemsOf(const std::map<uint32_t, Gem> &at)
        {
            std::vector<Gem> out;
            for (const auto &[tick, g] : at)
                if (g.lanes)
                    out.push_back(g);
            return out;
        }

        // [on, off) spans of one note number.
        using Spans = std::vector<Span>;

        bool within(const Span &s, uint32_t tick)
        {
            return tick >= s.tick && tick < s.tick + std::max(s.length, 1u);
        }

        const std::map<std::string, Part> kMidParts = {
            {"PART GUITAR", Part::Guitar}, {"T1 GEMS", Part::Guitar}, {"PART GUITAR COOP", Part::Coop},
            {"PART RHYTHM", Part::Rhythm}, {"PART BASS", Part::Bass},
        };

        std::array<Difficulty, 4> midPart(const midi::Track &track, uint32_t resolution, const Options &options)
        {
            std::map<uint8_t, Spans> notes;
            std::map<uint8_t, uint32_t> held;
            std::map<uint8_t, uint32_t> endedAt; // where a note struck again ended the held one
            // Phase Shift's opens by difficulty (4 all), sysex F0 'P' 'S' 0 0
            // <difficulty, FF all> 1 <1 on, 0 off> F7 over the green note.
            std::array<Spans, 5> opens;
            std::array<std::optional<uint32_t>, 5> openOn;
            for (const midi::Event &e : track.events)
            {
                if (e.status == midi::kSysex)
                {
                    const midi::Bytes &d = e.data;
                    const size_t diff = d.size() < 7u ? 5u : d[4] == 0xffu ? 4u : d[4];
                    if (diff > 4u || d[0] != 'P' || d[1] != 'S' || d[2] != 0u || d[3] != 0u || d[5] != 1u)
                        continue;
                    if (d[6] != 0u)
                        openOn[diff] = e.tick;
                    else if (openOn[diff])
                    {
                        opens[diff].push_back({*openOn[diff], e.tick - *openOn[diff]});
                        openOn[diff].reset();
                    }
                    continue;
                }
                const uint8_t type = midi::kind(e);
                if ((type != midi::kNoteOn && type != midi::kNoteOff) || e.data.size() < 2u)
                    continue;
                const uint8_t pitch = e.data[0];
                const bool down = type == midi::kNoteOn && e.data[1] != 0u;
                const auto it = held.find(pitch);
                // A note struck again while held ends the first there; that
                // note's own end, written after on the same tick, is the
                // first's and not the new one's.
                if (const auto ended = endedAt.find(pitch);
                    !down && it != held.end() && it->second == e.tick && ended != endedAt.end() && ended->second == e.tick)
                {
                    endedAt.erase(ended);
                    continue;
                }
                if (it != held.end())
                {
                    notes[pitch].push_back({it->second, e.tick - it->second});
                    held.erase(it);
                    if (down)
                        endedAt[pitch] = e.tick;
                }
                if (down)
                    held[pitch] = e.tick;
            }
            for (const auto &[pitch, on] : held)
                notes[pitch].push_back({on, 0u});

            const uint32_t cutoff = options.sustainCutoff.value_or(resolution / 3u);
            const bool spOn103 = options.starPowerOn103 || notes[116].empty();
            std::array<Difficulty, 4> out;
            for (size_t d = 0; d < 4u; ++d)
            {
                const uint8_t base = static_cast<uint8_t>(60u + 12u * d);
                std::map<uint32_t, Gem> at;
                for (int i = 0; i < 5; ++i)
                    for (const Span &s : notes[static_cast<uint8_t>(base + i)])
                    {
                        Gem &g = at[s.tick];
                        g.tick = s.tick;
                        g.lanes |= static_cast<uint8_t>(1u << i);
                        if (s.length > cutoff)
                            g.length = std::max(g.length, s.length);
                    }
                for (const size_t which : {d, size_t(4)})
                    for (const Span &o : opens[which])
                        std::erase_if(at, [&](const auto &g) { return within(o, g.first) && g.second.lanes == 1u; });
                out[d].gems = gemsOf(at);
                out[d].starPower = notes[spOn103 ? 103 : 116];
                if (!spOn103)
                    out[d].solos = notes[103];
            }
            return out;
        }

        bool hasGems(const std::array<Difficulty, 4> &part)
        {
            return std::any_of(part.begin(), part.end(), [](const Difficulty &d) { return !d.gems.empty(); });
        }

        std::string unquoted(std::string s)
        {
            s = ini::trim(s);
            if (s.size() >= 2u && s.front() == '"' && s.back() == '"')
                s = s.substr(1u, s.size() - 2u);
            return s;
        }

        // [ExpertSingle] as a part and difficulty.
        std::optional<std::pair<Part, size_t>> chartTrack(const std::string &name)
        {
            static const char *const kDiffs[] = {"Easy", "Medium", "Hard", "Expert"};
            static const std::pair<const char *, Part> kParts[] = {
                {"Single", Part::Guitar},
                {"DoubleGuitar", Part::Coop},
                {"DoubleRhythm", Part::Rhythm},
                {"DoubleBass", Part::Bass},
            };
            for (size_t d = 0; d < 4u; ++d)
                for (const auto &[suffix, part] : kParts)
                    if (name == std::string(kDiffs[d]) + suffix)
                        return std::pair{part, d};
            return std::nullopt;
        }

        // A section's practice name from an EVENTS text, or none.
        std::optional<std::string> sectionName(const std::string &text)
        {
            std::string s = text;
            if (s.size() >= 2u && s.front() == '[' && s.back() == ']')
                s = s.substr(1u, s.size() - 2u);
            for (const char *prefix : {"section ", "prc_"})
                if (s.rfind(prefix, 0) == 0)
                    return ini::trim(s.substr(std::char_traits<char>::length(prefix)));
            return std::nullopt;
        }

        // A .chart line's length field: a negative one none, a wild one cut
        // to what any song could hold.
        uint32_t lengthIn(std::istream &fields)
        {
            constexpr long long kMost = 1ll << 28;
            long long length = 0;
            fields >> length;
            return static_cast<uint32_t>(std::clamp(length, 0ll, kMost));
        }
    }

    double Chart::seconds(uint32_t tick) const
    {
        double at = 0.0;
        uint32_t from = 0u, us = 500000u;
        for (const Tempo &t : tempos)
        {
            if (t.tick >= tick)
                break;
            at += double(t.tick - from) * us / 1e6 / resolution;
            from = t.tick;
            us = t.usPerBeat;
        }
        return at + double(tick - from) * us / 1e6 / resolution;
    }

    std::optional<Chart> parseMid(const std::vector<uint8_t> &bytes, const Options &options, std::string &why)
    {
        const auto file = midi::parse(bytes);
        if (!file || file->tracks.empty())
        {
            why = "not a MIDI file";
            return std::nullopt;
        }
        if (file->division & 0x8000u)
        {
            why = "timed in SMPTE frames";
            return std::nullopt;
        }
        if (file->division == 0u)
        {
            why = "no ticks a beat";
            return std::nullopt;
        }
        Chart chart;
        chart.resolution = file->division;
        for (const midi::Event &e : file->tracks[0].events)
            if (e.status == midi::kMeta && e.meta == midi::kTempo && e.data.size() >= 3u)
            {
                // A tempo of 0 is none, as midi::TempoMap has it.
                if (const uint32_t us = uint32_t(e.data[0]) << 16 | uint32_t(e.data[1]) << 8 | e.data[2]; us != 0u)
                    chart.tempos.push_back({e.tick, us});
            }
            else if (e.status == midi::kMeta && e.meta == midi::kSignature && e.data.size() >= 2u && e.data[0] != 0u)
                chart.signatures.push_back({e.tick, {e.data[0], e.data[1]}});
        if (chart.tempos.empty() || chart.tempos.front().tick != 0u)
            chart.tempos.insert(chart.tempos.begin(), Tempo{});
        for (const midi::Track &track : file->tracks)
        {
            if (track.name == "EVENTS")
                for (const midi::Event &e : track.events)
                    if (e.status == midi::kMeta && e.meta == midi::kText)
                        if (auto name = sectionName(std::string(e.data.begin(), e.data.end())))
                            chart.sections.emplace_back(e.tick, *name);
            const auto part = kMidParts.find(track.name);
            if (part == kMidParts.end() || chart.parts.count(part->second))
                continue;
            auto read = midPart(track, chart.resolution, options);
            if (hasGems(read))
                chart.parts[part->second] = std::move(read);
        }
        return chart;
    }

    std::optional<Chart> parseChart(const std::string &text, const Options &options, std::string &why)
    {
        Chart chart;
        std::map<std::pair<Part, size_t>, std::map<uint32_t, Gem>> gems;
        std::map<std::pair<Part, size_t>, std::optional<uint32_t>> soloOn;
        std::istringstream in(ini::withoutBom(text));
        std::string line, section;
        while (std::getline(in, line))
        {
            line = ini::trim(line);
            if (line.empty() || line == "{" || line == "}")
                continue;
            if (const auto name = ini::sectionName(line))
            {
                section = *name;
                continue;
            }
            const size_t eq = line.find('=');
            if (eq == std::string::npos)
                continue;
            const std::string key = ini::trim(line.substr(0, eq)), value = ini::trim(line.substr(eq + 1u));
            if (section == "Song")
            {
                if (key == "Resolution")
                    chart.resolution = static_cast<uint32_t>(std::max(1, std::atoi(value.c_str())));
                else if (key == "Offset")
                    chart.offset = std::atof(value.c_str());
                continue;
            }
            const uint32_t tick = static_cast<uint32_t>(std::strtoul(key.c_str(), nullptr, 10));
            std::istringstream fields(value);
            std::string type;
            fields >> type;
            if (section == "SyncTrack")
            {
                if (type == "B")
                {
                    // Beats a minute in thousandths; at most a MIDI tempo's
                    // 24 bits of microseconds a beat (under 3.58 BPM).
                    double milli = 0.0;
                    fields >> milli;
                    if (milli > 0.0)
                        chart.tempos.push_back({tick, static_cast<uint32_t>(std::min(60e9 / milli + 0.5, double(0xffffffu)))});
                }
                else if (type == "TS")
                {
                    int num = 4, log2 = 2;
                    fields >> num;
                    if (!(fields >> log2))
                        log2 = 2;
                    if (num > 0 && num < 256 && log2 >= 0 && log2 < 8)
                        chart.signatures.push_back({tick, {static_cast<uint8_t>(num), static_cast<uint8_t>(log2)}});
                }
                continue;
            }
            if (section == "Events")
            {
                if (type == "E")
                    if (auto name = sectionName(unquoted(value.substr(1u))))
                        chart.sections.emplace_back(tick, *name);
                continue;
            }
            const auto track = chartTrack(section);
            if (!track)
                continue;
            if (type == "N")
            {
                int lane = -1;
                fields >> lane;
                const uint32_t length = lengthIn(fields);
                if (lane < 0 || lane > 7)
                    continue;
                // 5 to 7 (a kind flipped, a tap, an open) leave a gem
                // with no lane, which gemsOf drops.
                Gem &g = gems[*track][tick];
                g.tick = tick;
                if (lane > 4)
                    continue;
                g.lanes |= static_cast<uint8_t>(1u << lane);
                if (length > options.sustainCutoff.value_or(0u))
                    g.length = std::max(g.length, length);
            }
            else if (type == "S")
            {
                int kind = -1;
                fields >> kind;
                const uint32_t length = lengthIn(fields);
                if (kind == 2)
                    chart.parts[track->first][track->second].starPower.push_back({tick, length});
            }
            else if (type == "E")
            {
                std::string what;
                fields >> what;
                auto &on = soloOn[*track];
                if (what == "solo")
                    on = tick;
                else if (what == "soloend" && on)
                {
                    chart.parts[track->first][track->second].solos.push_back({*on, tick - *on});
                    on.reset();
                }
            }
        }
        std::stable_sort(chart.tempos.begin(), chart.tempos.end(), [](const Tempo &a, const Tempo &b) { return a.tick < b.tick; });
        std::stable_sort(chart.sections.begin(), chart.sections.end(),
                         [](const auto &a, const auto &b) { return a.first < b.first; });
        if (chart.tempos.empty() || chart.tempos.front().tick != 0u)
            chart.tempos.insert(chart.tempos.begin(), Tempo{});
        for (const auto &[track, at] : gems)
            chart.parts[track.first][track.second].gems = gemsOf(at);
        std::erase_if(chart.parts, [](const auto &p) { return !hasGems(p.second); });
        if (gems.empty() && chart.sections.empty() && chart.tempos.size() == 1u)
        {
            why = "not a .chart";
            return std::nullopt;
        }
        return chart;
    }

    std::optional<Chart> parseSongChart(const std::string &name, const std::string &bytes, const Options &options,
                                        std::string &why)
    {
        if (lower(std::filesystem::path(name).extension().string()) == ".mid")
            return parseMid({bytes.begin(), bytes.end()}, options, why);
        return parseChart(bytes, options, why);
    }
}
