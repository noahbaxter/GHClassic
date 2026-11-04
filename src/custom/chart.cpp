// notes.mid is Rock Band's form as Clone Hero reads it: each difficulty's
// lanes from note 60, 72, 84 and 96 (easy to expert), the next two its
// forced HOPO and forced strum, the one before its open where the track
// says [ENHANCED_OPENS]; 104 taps, 116 star power, 103 a solo (or, where
// nothing is on 116, star power: each difficulty's on the note past its
// frets, 67 to 103, as GH1 and GH2's, where those are); 105 and 106 each
// player's Face-Off phrases (or each difficulty's two past its star power,
// 69 and 70 to 105 and 106, as GH2's), 110 GH2's big notes, 120 a big rock
// ending, 126 and 127 tremolo and trill lanes (Expert's, Hard's too at
// velocity 41 to 50). Phase Shift's sysex, F0 'P' 'S' 0 0
// <difficulty, FF all> <1 open, 4 tap> <1 on, 0 off> F7, marks opens and
// taps too.
//
// notes.chart is text: [Song], [SyncTrack] (B a tempo in thousandths of a
// beat a minute, TS a signature), [Events], and a section a part and
// difficulty, [ExpertSingle], each line "<tick> = N <lane> <length>": 0
// to 4 the frets, 5 its natural kind flipped, 6 a tap, 7 open; "S 2
// <length>" star power, "S 0" and "S 1" each player's Face-Off phrases,
// "S 65" and "S 66" tremolo and trill lanes, "E solo" and "E soloend".
//
// Lengths are kept as written: each engine cuts its own short sustains as
// the song loads (gameplay/extended/guitar/gem_kinds.h).

#include "custom/chart.h"

#include "custom/text.h"
#include "formats/midi.h"
#include "settings/ini.h"

#include <algorithm>
#include <set>
#include <sstream>

namespace gh2::custom
{
    namespace
    {
        constexpr uint8_t kTremoloNote = 126u, kTrillNote = 127u;

        // A gem as read, before its kind is settled.
        struct Raw
        {
            Gem gem;
            bool hopo = false, strum = false, flip = false, tap = false;
        };

        // A tick at GH2's 480 a beat, as the converter rounds it.
        uint64_t at480(uint64_t tick, uint32_t resolution)
        {
            return (tick * 480u + resolution / 2u) / resolution;
        }

        // song.ini's threshold at 480, none where it is GH2's own.
        std::optional<uint32_t> songThreshold(const Options &options, uint32_t resolution)
        {
            uint32_t at = 0u;
            if (options.hopoFrequency)
                at = static_cast<uint32_t>(std::min<uint64_t>(at480(*options.hopoFrequency, resolution), 1u << 24));
            else if (options.eighthNoteHopo)
                at = 240u;
            if (at == 0u || at == kGh2HopoThreshold)
                return std::nullopt;
            return at;
        }

        // Notes starting within 10 ticks of a gem (at 480) are of it, as
        // GH2's parser makes chords (SongParser::ComputeSlots, 0x246088).
        std::vector<Raw> snapped(std::vector<Raw> raws, uint32_t resolution)
        {
            std::stable_sort(raws.begin(), raws.end(), [](const Raw &a, const Raw &b) { return a.gem.tick < b.gem.tick; });
            std::vector<Raw> out;
            out.reserve(raws.size());
            for (const Raw &r : raws)
            {
                if (out.empty() || at480(r.gem.tick, resolution) - at480(out.back().gem.tick, resolution) > 10u)
                {
                    out.push_back(r);
                    continue;
                }
                Raw &into = out.back();
                into.gem.lanes |= r.gem.lanes;
                for (size_t lane = 0; lane < into.gem.length.size(); ++lane)
                    into.gem.length[lane] = std::max(into.gem.length[lane], r.gem.length[lane]);
                into.hopo |= r.hopo, into.strum |= r.strum, into.flip |= r.flip, into.tap |= r.tap;
            }
            return out;
        }

        // Kinds, natural or forced, in order. Natural is GH2's (GameGemList::
        // WillBeNoStrum, 0x24d360): a single note on no fret of the gem
        // before, under the threshold after it. The engine judging decides
        // the natural ones and flips again as the song loads; an open's is
        // this one.
        std::vector<Gem> settle(std::vector<Raw> raws, uint32_t resolution, std::optional<uint32_t> threshold)
        {
            const uint32_t hopoTicks = threshold.value_or(kGh2HopoThreshold);
            std::vector<Gem> out;
            raws = snapped(std::move(raws), resolution);
            out.reserve(raws.size());
            for (Raw &r : raws)
            {
                Gem &g = r.gem;
                // An open with frets is the frets.
                if ((g.lanes & 0x1fu) != 0u && (g.lanes & (1u << kOpen)) != 0u)
                {
                    g.lanes &= 0x1fu;
                    g.length[kOpen] = 0u;
                }
                const Gem *last = out.empty() ? nullptr : &out.back();
                const bool natural = last && !g.chord() && (g.lanes & last->lanes) == 0u &&
                                     at480(g.tick, resolution) - at480(last->tick, resolution) < hopoTicks;
                g.kind = natural ? Kind::Hopo : Kind::Strum;
                if (r.tap)
                    g.kind = Kind::Tap;
                else if (r.hopo)
                    g.kind = Kind::Hopo;
                else if (r.strum)
                    g.kind = Kind::Strum;
                else if (r.flip)
                    g.kind = natural ? Kind::Strum : Kind::Hopo;
                g.forced = r.tap || r.hopo || r.strum;
                g.flip = r.flip && !g.forced;
                out.push_back(g);
            }
            return out;
        }

        // A lane's notes, as YARG picks them (MoonSongLoader.Guitar): a
        // tremolo one mask struck at least twice, cut where it changes; a
        // trill two single notes alternating, at least three, cut where
        // they stop.
        std::vector<Gem *> tremolo(const std::vector<Gem *> &notes)
        {
            if (notes.size() < 2u || notes[0]->lanes != notes[1]->lanes)
                return {};
            std::vector<Gem *> out;
            for (Gem *g : notes)
            {
                if (g->lanes != notes[0]->lanes)
                    break;
                out.push_back(g);
            }
            return out;
        }
        std::vector<Gem *> trill(const std::vector<Gem *> &notes)
        {
            if (notes.size() < 3u || notes[0]->chord() || notes[1]->chord() || notes[0]->lanes == notes[1]->lanes ||
                notes[2]->lanes != notes[0]->lanes)
                return {};
            std::vector<Gem *> out;
            for (size_t i = 0; i < notes.size() && notes[i]->lanes == notes[i % 2u]->lanes; ++i)
                out.push_back(notes[i]);
            return out;
        }

        // A difficulty's lane notes and bands. A lane holds the notes from
        // its start to its end, that tick too unless another lane starts
        // there (MoonSongLoader.GetNotesInLanePhrase). Its notes hold no
        // sustain but its last.
        void markLanes(Difficulty &d)
        {
            std::vector<std::pair<Span, bool>> lanes;
            for (const Span &s : d.tremolos)
                lanes.emplace_back(s, false);
            for (const Span &s : d.trills)
                lanes.emplace_back(s, true);
            std::stable_sort(lanes.begin(), lanes.end(), [](const auto &a, const auto &b) { return a.first.tick < b.first.tick; });
            const auto starts = [&](uint64_t tick)
            {
                return std::any_of(lanes.begin(), lanes.end(), [&](const auto &l) { return l.first.tick == tick; });
            };
            for (const auto &[s, isTrill] : lanes)
            {
                const uint64_t end = uint64_t(s.tick) + s.length;
                std::vector<Gem *> notes;
                for (Gem &g : d.gems)
                    if (g.tick >= s.tick && (g.tick < end || (g.tick == end && !starts(end))) && !g.laned)
                        notes.push_back(&g);
                const auto laned = isTrill ? trill(notes) : tremolo(notes);
                if (laned.empty())
                    continue;
                laned.front()->laneStart = true;
                std::map<int, std::pair<uint32_t, uint32_t>> reach;
                for (Gem *g : laned)
                {
                    g->laned = true;
                    if (g != laned.back())
                        g->length = {};
                    for (int lane = 0; lane <= kOpen; ++lane)
                        if (g->lanes & (1u << lane))
                        {
                            auto [it, added] = reach.try_emplace(lane, g->tick, g->tick);
                            it->second.second = g->tick;
                        }
                }
                for (const auto &[lane, ticks] : reach)
                    d.bands.push_back({ticks.first, ticks.second - ticks.first, lane});
            }
            std::stable_sort(d.bands.begin(), d.bands.end(), [](const auto &a, const auto &b) { return a.tick < b.tick; });
        }

        // [on, off) spans of one note number.
        using Spans = std::vector<Span>;

        bool within(const Spans &spans, uint32_t tick)
        {
            for (const Span &s : spans)
                if (tick >= s.tick && tick < s.tick + std::max(s.length, 1u))
                    return true;
            return false;
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
            // Lanes Hard has too: struck at velocity 41 to 50 (YARG's
            // MidReader), by note and tick.
            std::set<std::pair<uint8_t, uint32_t>> hardLanes;
            // Sysex phrases by difficulty (4 all), opens then taps.
            std::array<std::array<Spans, 5>, 2> phrases;
            std::array<std::array<std::optional<uint32_t>, 5>, 2> phraseOn;
            bool enhancedOpens = false;
            for (const midi::Event &e : track.events)
            {
                if (e.status == midi::kMeta)
                {
                    const std::string text(e.data.begin(), e.data.end());
                    if (e.meta == midi::kText && (text == "[ENHANCED_OPENS]" || text == "ENHANCED_OPENS"))
                        enhancedOpens = true;
                    continue;
                }
                if (e.status == midi::kSysex)
                {
                    const midi::Bytes &d = e.data;
                    if (d.size() < 7u || d[0] != 'P' || d[1] != 'S' || d[2] != 0u || d[3] != 0u)
                        continue;
                    const int which = d[5] == 1u ? 0 : d[5] == 4u ? 1 : -1;
                    const size_t diff = d[4] == 0xffu ? 4u : d[4];
                    if (which < 0 || diff > 4u)
                        continue;
                    auto &on = phraseOn[which][diff];
                    if (d[6] != 0u)
                        on = e.tick;
                    else if (on)
                    {
                        phrases[which][diff].push_back({*on, e.tick - *on});
                        on.reset();
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
                {
                    held[pitch] = e.tick;
                    if ((pitch == kTremoloNote || pitch == kTrillNote) && e.data[1] >= 41u && e.data[1] <= 50u)
                        hardLanes.insert({pitch, e.tick});
                }
            }
            for (const auto &[pitch, on] : held)
                notes[pitch].push_back({on, 0u});

            // Star power on 116 for all four (Rock Band's), else GH1 and
            // GH2's note a difficulty past its frets (67, 79, 91, 103),
            // else 103 for all four.
            const bool spOn103 = options.starPowerOn103 || notes[116].empty();
            const bool spEach = spOn103 && !(notes[67].empty() && notes[79].empty() && notes[91].empty());
            // Face-Off phrases, GH2's a difficulty (69 and 70 to 105 and
            // 106), else Rock Band's 105 and 106 for all four.
            const bool faceOffEach = !(notes[69].empty() && notes[70].empty() && notes[81].empty() &&
                                       notes[82].empty() && notes[93].empty() && notes[94].empty());
            std::array<Difficulty, 4> out;
            for (size_t d = 0; d < 4u; ++d)
            {
                const uint8_t base = static_cast<uint8_t>(60u + 12u * d);
                std::map<uint32_t, Raw> at;
                const auto lane = [&](uint8_t pitch, int index)
                {
                    for (const Span &s : notes[pitch])
                    {
                        Gem &g = at[s.tick].gem;
                        g.tick = s.tick;
                        g.lanes |= static_cast<uint8_t>(1u << index);
                        g.length[index] = s.length;
                    }
                };
                for (int i = 0; i < 5; ++i)
                    lane(static_cast<uint8_t>(base + i), i);
                if (enhancedOpens)
                    lane(static_cast<uint8_t>(base - 1u), kOpen);
                Spans opens = phrases[0][d], taps = phrases[1][d];
                opens.insert(opens.end(), phrases[0][4].begin(), phrases[0][4].end());
                taps.insert(taps.end(), phrases[1][4].begin(), phrases[1][4].end());
                taps.insert(taps.end(), notes[104].begin(), notes[104].end());
                std::vector<Raw> raws;
                for (auto &[tick, r] : at)
                {
                    // Phase Shift's open is the green lane's note.
                    if (within(opens, tick) && r.gem.lanes == 1u)
                    {
                        r.gem.lanes = 1u << kOpen;
                        r.gem.length[kOpen] = r.gem.length[0];
                        r.gem.length[0] = 0u;
                    }
                    r.hopo = within(notes[base + 5u], tick);
                    r.strum = within(notes[base + 6u], tick);
                    r.tap = within(taps, tick);
                    raws.push_back(r);
                }
                out[d].gems = settle(std::move(raws), resolution, songThreshold(options, resolution));
                out[d].starPower = notes[spEach ? base + 7u : spOn103 ? 103u : 116u];
                if (!spOn103)
                    out[d].solos = notes[103];
                for (uint8_t player = 0; player < 2u; ++player)
                    out[d].faceOff[player] = notes[(faceOffEach ? base + 9u : 105u) + player];
                out[d].bigNotes = notes[110];
                out[d].rockEndings = notes[120];
                // Lanes on Expert, and Hard where it has them too.
                for (const auto &[pitch, into] : {std::pair{kTremoloNote, &out[d].tremolos}, std::pair{kTrillNote, &out[d].trills}})
                    for (const Span &s : notes[pitch])
                        if (d == 3u || (d == 2u && hardLanes.count({pitch, s.tick})))
                            into->push_back(s);
                markLanes(out[d]);
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

    bool Gem::split() const
    {
        std::optional<uint32_t> first;
        for (int i = 0; i < 6; ++i)
            if (lanes & (1u << i))
            {
                if (first && *first != length[i])
                    return true;
                first = length[i];
            }
        return false;
    }

    uint32_t Gem::longest() const { return *std::max_element(length.begin(), length.end()); }

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
        chart.hopoThreshold = songThreshold(options, chart.resolution);
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
        std::map<std::pair<Part, size_t>, std::map<uint32_t, Raw>> raws;
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
                Raw &r = raws[*track][tick];
                r.gem.tick = tick;
                if (lane == 5)
                    r.flip = true;
                else if (lane == 6)
                    r.tap = true;
                else
                {
                    const int index = lane == 7 ? kOpen : lane;
                    r.gem.lanes |= static_cast<uint8_t>(1u << index);
                    r.gem.length[index] = length;
                }
            }
            else if (type == "S")
            {
                int kind = -1;
                fields >> kind;
                const uint32_t length = lengthIn(fields);
                Difficulty &diff = chart.parts[track->first][track->second];
                if (kind == 2)
                    diff.starPower.push_back({tick, length});
                else if (kind == 0 || kind == 1)
                    diff.faceOff[size_t(kind)].push_back({tick, length});
                else if (kind == 65)
                    diff.tremolos.push_back({tick, length});
                else if (kind == 66)
                    diff.trills.push_back({tick, length});
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
        chart.hopoThreshold = songThreshold(options, chart.resolution);
        for (auto &[track, gems] : raws)
        {
            std::vector<Raw> list;
            for (auto &[tick, r] : gems)
                // A flag with no lane under it is nothing.
                if (r.gem.lanes)
                    list.push_back(r);
            Difficulty &diff = chart.parts[track.first][track.second];
            diff.gems = settle(std::move(list), chart.resolution, chart.hopoThreshold);
            markLanes(diff);
        }
        std::erase_if(chart.parts, [](const auto &p) { return !hasGems(p.second); });
        if (raws.empty() && chart.sections.empty() && chart.tempos.size() == 1u)
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
