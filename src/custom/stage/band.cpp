// Each member of the band, as stage.h has them: when it plays, its hands,
// the drummer's kicks, crashes and beat, the bassist's pulse, the singer.

#include "custom/stage/parts.h"

#include <algorithm>
#include <bit>
#include <optional>

namespace gh2::custom
{
    namespace
    {
        // Every beat within the runs.
        std::vector<uint32_t> beatsIn(const std::vector<Span> &playing)
        {
            std::vector<uint32_t> out;
            for (const Span &r : playing)
                for (uint32_t t = nextBeat(r.tick); t <= r.tick + r.length; t += kBeat)
                    out.push_back(t);
            return out;
        }

        // When a GH2-made chart's own `name` track has its member play, by
        // its [play] and [idle]; none if it doesn't say.
        std::vector<Span> authoredPlaying(const Chart &chart, const char *name, const TickMap &toGh2, uint32_t end)
        {
            std::vector<std::pair<uint32_t, bool>> said;
            if (const Chart::StageTrack *track = stageTrack(chart, name))
                for (const Chart::Cue &c : track->cues)
                    if (c.text == "[play]" || c.text == "[idle]")
                        said.emplace_back(toGh2(c.tick), c.text == "[play]");
            std::sort(said.begin(), said.end());
            std::vector<Span> out;
            std::optional<uint32_t> from;
            for (const auto &[tick, play] : said)
                if (play && !from)
                    from = tick;
                else if (!play && from)
                {
                    out.push_back({*from, tick - *from});
                    from.reset();
                }
            if (from && end > *from)
                out.push_back({*from, end - *from});
            return out;
        }

        // A band member's own texts on its chart track, as GH2's: Rock
        // Band's ways of playing and idling as [play] and [idle], the rest of
        // kLaneTexts as they are, anything else left out.
        std::vector<Chart::Cue> memberCues(const Chart::StageTrack *track)
        {
            static const std::vector<std::string> kPlay = {"[play]", "[mellow]", "[intense]", "[play_solo]"},
                                                  kIdle = {"[idle]", "[idle_realtime]", "[idle_intense]"};
            const auto in = [](const std::vector<std::string> &list, const std::string &s)
            { return std::find(list.begin(), list.end(), s) != list.end(); };
            std::vector<Chart::Cue> out;
            for (const Chart::Cue &c : track ? track->cues : std::vector<Chart::Cue>{})
                if (in(kPlay, c.text))
                    out.push_back({c.tick, "[play]"});
                else if (in(kIdle, c.text))
                    out.push_back({c.tick, "[idle]"});
                else if (!gh2Lane(c.text).empty())
                    out.push_back(c);
            return out;
        }

        bool has(const std::vector<Chart::Cue> &cues, const char *text)
        {
            return std::any_of(cues.begin(), cues.end(), [&](const Chart::Cue &c) { return c.text == text; });
        }

        // A member's texts on `out`: the chart's own [play] and [idle] where
        // it has them, else [play] a half beat before each of `playing` and
        // [idle] a beat past it; its other cues either way.
        void addMemberCues(midi::Track &out, const std::vector<Chart::Cue> &cues, const std::vector<Span> &playing,
                           const TickMap &toGh2)
        {
            const bool chartSaysWhenToPlay = has(cues, "[play]");
            if (!chartSaysWhenToPlay)
                for (const Span &r : playing)
                {
                    out.events.push_back(midi::text(r.tick > kBeat / 2u ? r.tick - kBeat / 2u : 0u, "[play]"));
                    out.events.push_back(midi::text(r.tick + r.length + kBeat, "[idle]"));
                }
            for (const Chart::Cue &c : cues)
                if (chartSaysWhenToPlay || (c.text != "[play]" && c.text != "[idle]"))
                    out.events.push_back(midi::text(toGh2(c.tick), c.text));
        }

        // A [map ...] as GH2 has it for a player's hands: its own hand and
        // strum maps (midi_parsers.dta), another hand map as its default,
        // another strum map (Rock Band's pick, the bass's slap) as none.
        std::optional<std::string> guitarMap(const std::string &text)
        {
            static const std::vector<std::string> kHands = {"Default", "Linear", "NoChords", "AllChords",
                                                            "DropD",   "DropD2", "Solo"},
                                                  kStrums = {"Default", "punk", "softpick"};
            const std::string hand = "[map HandMap_", strum = "[map StrumMap_";
            const auto named = [&](const std::string &prefix, const std::vector<std::string> &names)
            {
                const std::string name = text.substr(prefix.size(), text.size() - prefix.size() - 1u);
                return std::find(names.begin(), names.end(), name) != names.end();
            };
            if (text.back() != ']')
                return std::nullopt;
            if (text.rfind(hand, 0) == 0)
                return named(hand, kHands) ? text : "[map HandMap_Default]";
            if (text.rfind(strum, 0) == 0 && named(strum, kStrums))
                return text;
            return std::nullopt;
        }

        uint64_t fnv(uint64_t h, uint64_t v)
        {
            for (int i = 0; i < 8; ++i, v >>= 8u)
                h = (h ^ (v & 0xffu)) * 0x100000001b3ull;
            return h;
        }
        constexpr uint64_t kFnvBasis = 0xcbf29ce484222325ull;

        // Where the fret hand sits (notes 40 to 59, the neck's frets 1 to
        // 20), made where a chart has none. A chart's lanes wrap around and
        // say nothing of real notes, so the hand takes one place for each
        // part of the song: the chart's sections, else runs of the bars it
        // repeats (each bar by its rhythm and its lanes' shape, so a riff
        // moved up the lanes is the same riff); solos are parts of their
        // own. Each kind of part (a section's name less its number, the bars
        // repeated) always takes the same place, the busier the higher:
        // frets 1 to 9, solos and parts mostly tapped 10 to 14. It moves
        // only at a part's start, a part under two bars is the one before's,
        // and a part of only open notes or rests leaves it where it is.
        // Each kind of part also takes a hand map (kOpenHandMap's): Solo
        // in a solo, AllChords where half its notes are chords, Linear
        // where three in five single notes are a lane from the one before
        // (trills and runs, a finger a lane), else Default. `places` and
        // `maps` say which to make.
        void fretHand(BandTracks &band, bool places, bool maps)
        {
            const Difficulty &lead = band.in.lead;
            const auto &toGh2 = band.in.toGh2;
            const std::vector<uint32_t> &bars = band.bars;
            const size_t n = bars.size();
            if (n == 0u)
                return;
            const auto barOf = [&](uint32_t tick)
            {
                const auto i = size_t(std::upper_bound(bars.begin(), bars.end(), tick) - bars.begin());
                return i ? i - 1u : 0u;
            };
            std::vector<uint64_t> shape(n, kFnvBasis);
            std::vector<int> lowest(n, 5);
            for (const Gem &g : lead.gems)
                if (g.lanes & kFretLanes)
                    lowest[barOf(toGh2(g.tick))] = std::min(lowest[barOf(toGh2(g.tick))], std::countr_zero(g.lanes));
            for (const Gem &g : lead.gems)
            {
                const uint32_t at = toGh2(g.tick);
                const size_t b = barOf(at);
                const unsigned frets = (g.lanes & kFretLanes) >> (lowest[b] % 5);
                shape[b] = fnv(fnv(shape[b], (at - bars[b]) / (kBeat / 8u)), frets | (g.lanes & ~kFretLanes));
            }

            struct Chunk
            {
                uint32_t from;
                uint64_t kind;
                bool solo = false, unrepeated = false;
            };
            std::vector<Chunk> parts;
            if (band.in.chart.sections.size() >= 2u)
                for (const auto &[tick, name] : band.in.chart.sections)
                {
                    uint64_t kind = kFnvBasis;
                    for (const char c : sectionBaseName(name))
                        kind = fnv(kind, uint8_t(c));
                    parts.push_back({toGh2(tick), kind});
                }
            else
                // Greedily, the longest run of 8, 4 or 2 bars from here that
                // the song plays again elsewhere, by the riff it repeats
                // (four bars of one riff as two of it); bars repeated
                // nowhere a part of their own.
                for (size_t i = 0; i < n;)
                {
                    size_t length = 0u;
                    for (const size_t l : {8u, 4u, 2u})
                        for (size_t j = 0; !length && i + l <= n && j + l <= n; ++j)
                            if ((j + l <= i || j >= i + l) && std::equal(&shape[i], &shape[i] + l, &shape[j]) &&
                                std::any_of(&shape[i], &shape[i] + l, [](uint64_t s) { return s != kFnvBasis; }))
                                length = l;
                    size_t period = 1u;
                    while (length && !std::equal(shape.data() + i + period, shape.data() + i + length, shape.data() + i))
                        ++period;
                    uint64_t kind = length ? kFnvBasis : fnv(kFnvBasis, ~uint64_t(i));
                    for (size_t k = 0; k < period && length; ++k)
                        kind = fnv(kind, shape[i + k]);
                    if (length || parts.empty() || !parts.back().unrepeated)
                        parts.push_back({bars[i], kind, false, !length});
                    i += std::max<size_t>(length, 1u);
                }
            std::sort(parts.begin(), parts.end(), [](const Chunk &a, const Chunk &b) { return a.from < b.from; });
            for (const Span &s : lead.solos)
            {
                const uint32_t from = toGh2(s.tick), to = toGh2(s.tick + s.length);
                const auto at = [&](uint32_t t)
                { return std::upper_bound(parts.begin(), parts.end(), t, [](uint32_t t, const Chunk &p) { return t < p.from; }); };
                const uint64_t after = at(to) == parts.begin() ? 0u : std::prev(at(to))->kind;
                const uint64_t kind = at(from) == parts.begin() ? 0u : std::prev(at(from))->kind;
                std::erase_if(parts, [&](const Chunk &p) { return p.from >= from && p.from <= to; });
                parts.insert(at(from), {from, fnv(kind, 1u), true});
                parts.insert(at(to), {to, after});
            }
            // A part under two bars is the one before's, as is one of the
            // same kind; the first starts with the song.
            for (size_t i = 0; i + 1u < parts.size();)
                if (barOf(parts[i + 1u].from) < barOf(parts[i].from) + 2u)
                    parts.erase(parts.begin() + std::ptrdiff_t(i));
                else
                    ++i;
            for (size_t i = 1; i < parts.size();)
                if (parts[i].kind == parts[i - 1u].kind)
                    parts.erase(parts.begin() + std::ptrdiff_t(i));
                else
                    ++i;
            if (parts.empty())
                return;
            parts.front().from = 0u;

            struct Place
            {
                // Its fretted notes, taps, chords, and single notes after a
                // single note, and of those a lane from it.
                unsigned notes = 0u, taps = 0u, chords = 0u, singlePairs = 0u, steps = 0u;
                uint32_t ticks = 0u;
                bool solo = false;
                uint8_t fret = 0u;
                const char *map() const
                {
                    return solo                        ? "Solo"
                           : chords * 2u >= notes      ? "AllChords"
                           : steps * 5u >= singlePairs * 3u ? "Linear"
                                                       : "Default";
                }
            };
            std::map<uint64_t, Place> kinds;
            std::vector<uint64_t> order; // kinds by first part
            std::vector<unsigned> partNotes(parts.size(), 0u);
            for (size_t i = 0; i < parts.size(); ++i)
            {
                const uint32_t to = i + 1u < parts.size() ? parts[i + 1u].from : band.in.end;
                if (!kinds.count(parts[i].kind))
                    order.push_back(parts[i].kind);
                Place &k = kinds[parts[i].kind];
                int lastSingle = -1;
                for (const Gem &g : lead.gems)
                    if (const uint32_t at = toGh2(g.tick); at >= parts[i].from && at < to && (g.lanes & kFretLanes))
                    {
                        ++partNotes[i];
                        k.taps += g.kind == Kind::Tap;
                        const int lane = std::countr_zero(g.lanes);
                        if (g.chord())
                            ++k.chords;
                        else if (lastSingle >= 0)
                        {
                            ++k.singlePairs;
                            k.steps += std::abs(lane - lastSingle) == 1;
                        }
                        lastSingle = g.chord() ? -1 : lane;
                    }
                k.notes += partNotes[i];
                k.ticks += to > parts[i].from ? to - parts[i].from : 0u;
                k.solo |= parts[i].solo;
            }
            for (bool high : {false, true})
            {
                std::vector<Place *> ranked;
                for (const uint64_t key : order)
                    if (Place &k = kinds[key]; k.notes && (k.solo || k.taps * 2u >= k.notes) == high)
                        ranked.push_back(&k);
                std::stable_sort(ranked.begin(), ranked.end(), [](const Place *a, const Place *b)
                                 { return double(a->notes) * b->ticks < double(b->notes) * a->ticks; });
                const unsigned lo = high ? 10u : 1u, hi = high ? 14u : 9u;
                for (size_t r = 0; r < ranked.size(); ++r)
                    ranked[r]->fret = static_cast<uint8_t>(
                        ranked.size() < 2u ? (lo + hi) / 2u : lo + (r * (hi - lo) + (ranked.size() - 1u) / 2u) / (ranked.size() - 1u));
            }
            uint8_t last = 0u;
            std::string lastMap;
            // A song opening on opens alone has them as finger_open too.
            if (maps && !partNotes[0])
            {
                lastMap = "Default";
                band.guitar.events.push_back(midi::text(0u, "[map HandMap_" + lastMap + kOpenHandMap + "]"));
            }
            for (size_t i = 0; i < parts.size(); ++i)
            {
                const Place &k = kinds[parts[i].kind];
                if (!partNotes[i])
                    continue;
                if (places && k.fret != last)
                    addShortNote(band.guitar, parts[i].from, static_cast<uint8_t>(39u + k.fret));
                if (maps && k.map() != lastMap)
                    band.guitar.events.push_back(
                        midi::text(parts[i].from, std::string("[map HandMap_") + k.map() + kOpenHandMap + "]"));
                last = k.fret;
                lastMap = k.map();
            }
        }

        enum class DrumPattern
        {
            All,
            Double,
            Half,
            Nobeat,
        };

        constexpr const char *kPatternTexts[] = {"[allbeat]", "[double_time]", "[half_time]", "[nobeat]"}; // by DrumPattern

        // A bar's drum pattern from its snares, as GH2's drummer clips
        // (kBandAllbeat, kBandDouble, kBandHalf, kBandNosnare):
        //   no snares                                       [nobeat]
        //   one on every beat, or two or more off the beat  [double_time]
        //   on beat 3 of a 4-beat bar but not 2 or 4        [half_time]
        //   anything else, a backbeat                       [allbeat]
        // `length` is the band's beat.
        DrumPattern barPattern(const std::vector<uint32_t> &snares, uint32_t from, uint32_t to, uint32_t length)
        {
            const uint32_t beatsInBar = std::max(1u, (to - from + length / 2u) / length);
            // How early a snare can be and still be on its beat.
            const uint32_t slack = length / 8u;
            std::vector<bool> on(beatsInBar, false);
            unsigned offbeats = 0u, count = 0u;
            for (auto it = std::lower_bound(snares.begin(), snares.end(), from); it != snares.end() && *it < to; ++it)
            {
                ++count;
                const uint32_t into = *it - from, beat = (into + slack) / length;
                if (beat < beatsInBar && into + slack - beat * length < length / 4u)
                    on[beat] = true;
                else
                    ++offbeats;
            }
            if (count == 0u)
                return DrumPattern::Nobeat;
            if (std::count(on.begin(), on.end(), true) == static_cast<std::ptrdiff_t>(beatsInBar) || offbeats >= 2u)
                return DrumPattern::Double;
            if (beatsInBar == 4u && on[2] && !on[1] && !on[3])
                return DrumPattern::Half;
            return DrumPattern::All;
        }

        // The drum pattern bar by bar while the drums play, a change kept
        // only where it holds two bars; [allbeat] at each run's start where
        // there are no charted drums to say.
        void addDrumPatterns(midi::Track &drums, const std::vector<uint32_t> &bars, const std::vector<uint32_t> &snares,
                             const std::vector<Span> &drumming, bool charted, uint32_t end, const BandBeatLengths &beats)
        {
            if (!charted)
            {
                for (const Span &r : drumming)
                    drums.events.push_back(midi::text(r.tick, "[allbeat]"));
                return;
            }
            for (const Span &r : drumming)
            {
                std::vector<std::pair<uint32_t, DrumPattern>> run;
                for (size_t b = 0; b < bars.size(); ++b)
                {
                    const uint32_t to = b + 1u < bars.size() ? bars[b + 1u] : end;
                    if (to > r.tick && bars[b] <= r.tick + r.length)
                        run.push_back({std::max(bars[b], r.tick), barPattern(snares, bars[b], to, beatAt(beats, bars[b]))});
                }
                std::optional<DrumPattern> now;
                for (size_t i = 0; i < run.size(); ++i)
                {
                    const DrumPattern pattern = run[i].second;
                    const bool holds = i + 1u >= run.size() || run[i + 1u].second == pattern;
                    if (pattern != now && (!now || holds))
                    {
                        drums.events.push_back(midi::text(run[i].first, kPatternTexts[size_t(pattern)]));
                        now = pattern;
                    }
                }
            }
        }

        // The lead's playing runs with its solos cut out.
        std::vector<Span> playingOutsideSolos(const BandTracks &band)
        {
            std::vector<Span> out;
            for (const Span &r : band.playing)
            {
                uint32_t from = r.tick;
                const uint32_t to = r.tick + r.length;
                for (const Span &solo : band.in.lead.solos)
                {
                    const uint32_t soloFrom = band.in.toGh2(solo.tick), soloTo = band.in.toGh2(solo.tick + solo.length);
                    if (soloTo <= from || soloFrom >= to)
                        continue;
                    if (soloFrom > from)
                        out.push_back({from, soloFrom - from});
                    from = std::max(from, soloTo);
                }
                if (to > from)
                    out.push_back({from, to - from});
            }
            return out;
        }
    }

    midi::Track bassHand(const Chart &chart, const TickMap &toGh2)
    {
        midi::Track out;
        out.name = "PART BASS";
        const auto it = chart.parts.find(Part::Bass);
        if (it == chart.parts.end())
            return out;
        for (size_t d = 0; d < 4u; ++d)
            for (const Gem &g : it->second[d].gems)
            {
                const uint8_t frets = (g.lanes & kFretLanes) ? static_cast<uint8_t>(g.lanes & kFretLanes) : 1u;
                const uint32_t start = toGh2(g.tick), end = std::max(toGh2(g.tick + g.longest()), start + kBeat / 8u);
                for (uint8_t lane = 0; lane < 5u; ++lane)
                    if (frets & (1u << lane))
                        addNote(out, start, end, static_cast<uint8_t>(60u + 12u * d + lane));
            }
        if (const auto *track = stageTrack(chart, "PART BASS"))
            for (const Chart::Cue &c : track->cues)
                if (c.text.rfind("[map StrumMap_", 0) == 0)
                    out.events.push_back(midi::text(toGh2(c.tick), c.text == "[map StrumMap_SlapBass]"
                                                                       ? c.text
                                                                       : "[map StrumMap_Default]"));
        return out;
    }

    void addGuitarist(BandTracks &band)
    {
        const auto &toGh2 = band.in.toGh2;
        const Chart::StageTrack *track = stageTrack(band.in.chart, trackOf(band.in.part));
        band.guitaristCues = memberCues(track);
        addMemberCues(band.guitar, band.guitaristCues, band.playing, toGh2);
        bool placed = false, mapped = false;
        if (track)
        {
            for (const Chart::Mark &m : track->marks)
                if (m.pitch >= 40u && m.pitch <= 59u)
                {
                    addNote(band.guitar, toGh2(m.tick), toGh2(m.tick + m.length), m.pitch);
                    placed = true;
                }
            for (const Chart::Cue &c : track->cues)
                if (const auto map = guitarMap(c.text))
                {
                    band.guitar.events.push_back(midi::text(toGh2(c.tick), *map));
                    mapped |= map->rfind("[map HandMap_", 0) == 0;
                }
        }
        fretHand(band, !placed, !mapped);
    }

    std::vector<uint32_t> addDrummer(BandTracks &band)
    {
        const Chart &chart = band.in.chart;
        const auto &toGh2 = band.in.toGh2;
        band.drums.name = "BAND DRUMS";
        const std::vector<Span> authored = authoredPlaying(chart, "BAND DRUMS", toGh2, band.in.end);
        const std::vector<Span> drumming = !chart.drums.empty() ? playingRuns(toGh2Sorted(chart.drums, toGh2))
                                           : !authored.empty()  ? authored
                                                                : band.playing;
        std::vector<uint32_t> kicks; // the chart's own BAND DRUMS kicks, else made
        if (const Chart::StageTrack *track = stageTrack(chart, "BAND DRUMS"))
            for (const Chart::Mark &m : track->marks)
                if (m.pitch == 36u)
                    kicks.push_back(toGh2(m.tick));
        std::sort(kicks.begin(), kicks.end());
        if (kicks.empty())
            kicks = chart.kicks.empty() ? beatsIn(drumming) : toGh2Sorted(chart.kicks, toGh2);
        const std::vector<Chart::Cue> drummerCues = memberCues(stageTrack(chart, "PART DRUMS"));
        addMemberCues(band.drums, drummerCues, drumming, toGh2);
        for (const uint32_t t : kicks)
            addShortNote(band.drums, t, 36u);
        // The chart's crashes, else one a section (addSectionCrashes).
        for (const uint32_t t : toGh2Sorted(chart.crashes, toGh2))
            addShortNote(band.drums, t, 37u);
        if (std::none_of(drummerCues.begin(), drummerCues.end(),
                         [](const Chart::Cue &c) { return gh2Lane(c.text) == "beat"; }))
            addDrumPatterns(band.drums, band.bars, toGh2Sorted(chart.snares, toGh2), drumming, !chart.drums.empty(),
                            band.in.end, band.beats);
        return kicks;
    }

    void addBassist(BandTracks &band, const std::vector<uint32_t> &kicks)
    {
        const Chart &chart = band.in.chart;
        band.bass.name = "BAND BASS";
        std::vector<uint32_t> pulses = kicks;
        if (const auto it = chart.parts.find(Part::Bass); it != chart.parts.end() && !it->second[3].gems.empty())
            pulses = toGh2Sorted(ticksOf(it->second[3].gems), band.in.toGh2);
        addMemberCues(band.bass, memberCues(stageTrack(chart, "PART BASS")), playingRuns(pulses), band.in.toGh2);
        for (const uint32_t t : pulses)
            addShortNote(band.bass, t, 36u);
    }

    void addKeys(BandTracks &band)
    {
        const Chart &chart = band.in.chart;
        band.keys.name = "BAND KEYS";
        if (!chart.keys.empty())
            addMemberCues(band.keys, memberCues(stageTrack(chart, "PART KEYS")),
                          playingRuns(toGh2Sorted(chart.keys, band.in.toGh2)), band.in.toGh2);
    }

    void addSinger(BandTracks &band)
    {
        const Chart &chart = band.in.chart;
        const auto &toGh2 = band.in.toGh2;
        band.singer.name = "BAND SINGER";
        std::vector<Chart::Cue> singerCues = memberCues(stageTrack(chart, "PART VOCALS"));
        if (!has(singerCues, "[play]"))
            singerCues = memberCues(stageTrack(chart, "HARM1"));
        std::vector<Span> singerSpans;
        for (const Span &s : chart.vocals)
            singerSpans.push_back({toGh2(s.tick), toGh2(s.tick + s.length) - toGh2(s.tick)});
        if (chart.vocals.empty())
            singerSpans = playingOutsideSolos(band);
        addMemberCues(band.singer, singerCues, {}, toGh2);
        // GH2's own singers never idle under three beats: a shorter rest
        // between phrases is sung through.
        std::vector<Span> held;
        for (const Span &s : singerSpans)
            if (!held.empty() && s.tick < held.back().tick + held.back().length + 3u * kBeat)
                held.back().length = std::max(held.back().length, s.tick + s.length - held.back().tick);
            else
                held.push_back(s);
        if (!has(singerCues, "[play]"))
            for (const Span &s : held)
            {
                band.singer.events.push_back(midi::text(s.tick, "[play]"));
                band.singer.events.push_back(midi::text(s.tick + s.length, "[idle]"));
            }
        if (band.in.singing)
            *band.in.singing = singerSpans;
    }

    void addSectionCrashes(BandTracks &band, const std::vector<Section> &sections)
    {
        if (!band.in.chart.crashes.empty())
            return;
        for (const Section &s : sections)
            if (!band.in.chart.drums.empty() || s.mood != Mood::Verse)
                addShortNote(band.drums, s.tick, 37u);
    }

    void addWails(BandTracks &band, const std::vector<Section> &sections)
    {
        if (has(band.guitaristCues, "[wail_on]"))
            return;
        const std::vector<uint32_t> &bars = band.bars;
        for (size_t i = 0; i < sections.size(); ++i)
        {
            const Section &s = sections[i];
            const auto bar = std::lower_bound(bars.begin(), bars.end(), s.tick);
            const uint32_t next = i + 1u < sections.size() ? sections[i + 1u].tick : band.in.end;
            const uint32_t to = std::min(bars.end() - bar > 4 ? *(bar + 4) : band.in.end, next);
            const bool guitaristPlaying = std::any_of(band.playing.begin(), band.playing.end(), [&](const Span &r)
                                                      { return s.tick >= r.tick && s.tick < r.tick + r.length; });
            if (s.mood == Mood::Chorus && guitaristPlaying && to > s.tick)
            {
                band.guitar.events.push_back(midi::text(s.tick, "[wail_on]"));
                band.guitar.events.push_back(midi::text(to, "[wail_off]"));
            }
        }
    }
}
