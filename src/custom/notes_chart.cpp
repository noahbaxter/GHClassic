// notes.chart is text: [Song], [SyncTrack] (B a tempo in thousandths of a
// beat a minute, TS a signature), [Events], and a section a part and
// difficulty, [ExpertSingle], each line "<tick> = N <lane> <length>": 0
// to 4 the frets, 5 its natural kind flipped, 6 a tap, 7 open; "S 2
// <length>" star power, "S 0" and "S 1" each player's Face-Off phrases,
// "S 65" and "S 66" tremolo and trill phrases, "E solo" and "E soloend".

#include "custom/chart_rules.h"

#include "settings/ini.h"

#include <set>
#include <sstream>

namespace gh2::custom
{
    using namespace rules;

    namespace
    {
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

    std::optional<Chart> parseChart(const std::string &text, const Options &options, std::string &why)
    {
        Chart chart;
        std::map<std::pair<Part, size_t>, std::map<uint32_t, Raw>> rawsByTrack;
        std::map<std::pair<Part, size_t>, std::optional<uint32_t>> soloOn;
        std::optional<uint32_t> phraseOn;
        std::vector<uint32_t> lyrics;
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
                {
                    const std::string text = unquoted(value.substr(1u));
                    if (!text.empty())
                        chart.stage["EVENTS"].cues.push_back({tick, text});
                    if (auto name = sectionName(text))
                        chart.sections.emplace_back(tick, *name);
                    else if (text == "phrase_start")
                        phraseOn = tick;
                    else if (text == "phrase_end" && phraseOn)
                    {
                        chart.vocals.push_back({*phraseOn, tick - *phraseOn});
                        phraseOn.reset();
                    }
                    else if (text.rfind("lyric", 0) == 0)
                        lyrics.push_back(tick);
                }
                continue;
            }
            // 0 the kick (32 the second pedal's), 1 to 5 the pads.
            if (section == "ExpertDrums")
            {
                int lane = -1;
                if (type == "N" && (fields >> lane) && (lane == 0 || lane == 32 || (lane >= 1 && lane <= 5)))
                {
                    if (lane == 0 || lane == 32)
                        chart.kicks.push_back(tick);
                    if (lane == 1)
                        chart.snares.push_back(tick);
                    chart.drums.push_back(tick);
                }
                continue;
            }
            if (section == "ExpertKeyboard")
            {
                int lane = -1;
                if (type == "N" && (fields >> lane) && lane >= 0 && lane <= 4)
                    chart.keys.push_back(tick);
                continue;
            }
            const auto track = chartTrack(section);
            if (!track)
                continue;
            if (type == "N")
            {
                int lane = -1;
                if (!(fields >> lane) || lane < 0 || lane > 7)
                    continue;
                const uint32_t length = lengthIn(fields);
                Raw &r = rawsByTrack[*track][tick];
                r.gem.tick = tick;
                if (lane == 5)
                    r.flip = true;
                else if (lane == 6)
                    r.tap = true;
                else
                    r.add(lane == 7 ? kOpen : lane, length);
            }
            else if (type == "S")
            {
                int phrase = -1;
                if (!(fields >> phrase))
                    continue;
                const uint32_t length = lengthIn(fields);
                Difficulty &diff = chart.parts[track->first][track->second];
                if (phrase == 2)
                    diff.starPower.push_back({tick, length});
                else if (phrase == 0 || phrase == 1)
                    diff.faceOff[size_t(phrase)].push_back({tick, length});
                else if (phrase == 65)
                    diff.tremolos.push_back({tick, length});
                else if (phrase == 66)
                    diff.trills.push_back({tick, length});
            }
            else if (type == "E")
            {
                std::string event;
                fields >> event;
                auto &on = soloOn[*track];
                if (event == "solo")
                    on = tick;
                else if (event == "soloend" && on)
                {
                    chart.parts[track->first][track->second].solos.push_back({*on, tick - *on});
                    on.reset();
                }
            }
        }
        chart.hopoThreshold = songThreshold(options, chart.resolution);
        for (const auto &[track, raws] : rawsByTrack)
            settleGems(chart.parts[track.first][track.second], raws, chart.resolution, chart.hopoThreshold);
        std::erase_if(chart.parts, [](const auto &p) { return !hasGems(p.second); });
        if (chart.vocals.empty())
            chart.vocals = phrasesFrom(lyrics, chart.resolution);
        finish(chart);
        if (rawsByTrack.empty() && chart.sections.empty() && chart.tempos.size() == 1u)
        {
            why = "not a .chart";
            return std::nullopt;
        }
        return chart;
    }
}
