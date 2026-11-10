#include "custom/stage/stage.h"

#include "custom/gh2_chart.h"

#include <algorithm>
#include <bit>
#include <cctype>
#include <map>
#include <sstream>

namespace gh2::custom
{
    namespace
    {
        using TickMap = std::function<uint32_t(uint32_t)>;

        constexpr uint32_t kBeat = kGh2Resolution;
        // A gap of 8+ beats between notes ends a playing run.
        constexpr uint32_t kRestEndsRun = 8u * kBeat;
        // A section whose name says nothing is a chorus when this much busier
        // than the median section.
        constexpr double kBusier = 1.2;

        // A sixteenth note.
        void addShortNote(midi::Track &track, uint32_t tick, uint8_t pitch) { addNote(track, tick, tick + kBeat / 4u, pitch); }

        uint32_t nameHash(const std::string &s)
        {
            uint32_t h = 2166136261u;
            for (const unsigned char c : s)
                h = (h ^ c) * 16777619u;
            return h;
        }

        // `ticks` as playing runs, each to its last tick.
        std::vector<Span> playingRuns(const std::vector<uint32_t> &ticks) { return runs(ticks, kRestEndsRun, 0u); }

        // Every beat within the runs.
        std::vector<uint32_t> beatsIn(const std::vector<Span> &playing)
        {
            std::vector<uint32_t> out;
            for (const Span &r : playing)
                for (uint32_t t = (r.tick + kBeat - 1u) / kBeat * kBeat; t <= r.tick + r.length; t += kBeat)
                    out.push_back(t);
            return out;
        }

        // Each bar's start in GH2's ticks, from 0 to `end`, or to where `at`
        // stops going on (a wild chart's end is past the most it maps to).
        std::vector<uint32_t> barsTo(const Chart &chart, const std::function<uint32_t(uint32_t)> &at, uint32_t end)
        {
            std::vector<uint32_t> out;
            uint32_t tick = 0u, length = chart.resolution * 4u;
            size_t next = 0u;
            while (at(tick) < end && (out.empty() || at(tick) > out.back()))
            {
                while (next < chart.signatures.size() && chart.signatures[next].first <= tick)
                {
                    const auto &sig = chart.signatures[next++].second;
                    length = std::max(1u, chart.resolution * 4u * sig[0] / (1u << std::min<uint8_t>(sig[1], 6u)));
                }
                out.push_back(at(tick));
                tick += length;
            }
            return out;
        }

        enum class Look
        {
            Verse,
            Chorus,
            Solo,
        };

        // Where the name says.
        std::optional<Look> named(const std::string &look)
        {
            if (look.find("solo") != std::string::npos)
                return Look::Solo;
            for (const char *word : {"chorus", "refrain", "hook"})
                if (look.find(word) != std::string::npos)
                    return Look::Chorus;
            for (const char *word : {"verse", "intro", "outro", "bridge", "break", "interlude", "pre"})
                if (look.find(word) != std::string::npos)
                    return Look::Verse;
            return std::nullopt;
        }

        // Its lighting, the same for each look by name.
        std::string lighting(Look look, const std::string &name)
        {
            static const std::vector<std::string> kVerse = {"()", "(color1)", "(sweep)"}, kChorus = {"(color2)", "(chase)"},
                                                  kSolo = {"(strobe)"};
            const auto &from = look == Look::Verse ? kVerse : look == Look::Chorus ? kChorus : kSolo;
            return "[lighting " + from[nameHash(name) % from.size()] + "]";
        }

        const char *cue(Look look) { return look == Look::Verse ? "[verse]" : look == Look::Chorus ? "[chorus]" : "[solo]"; }

        std::vector<uint32_t> ticksOf(const std::vector<Gem> &gems)
        {
            std::vector<uint32_t> out;
            for (const Gem &g : gems)
                out.push_back(g.tick);
            return out;
        }

        // `ticks` in GH2 ticks, sorted, each once.
        std::vector<uint32_t> toGh2Sorted(const std::vector<uint32_t> &ticks, const TickMap &toGh2)
        {
            std::vector<uint32_t> out;
            for (const uint32_t t : ticks)
                out.push_back(toGh2(t));
            std::sort(out.begin(), out.end());
            out.erase(std::unique(out.begin(), out.end()), out.end());
            return out;
        }

        // The chart's track a part's own texts and hand are on.
        const char *trackOf(Part part)
        {
            switch (part)
            {
            case Part::Coop: return "PART GUITAR COOP";
            case Part::Rhythm: return "PART RHYTHM";
            case Part::Bass: return "PART BASS";
            default: return "PART GUITAR";
            }
        }

        const Chart::StageTrack *stageTrack(const Chart &chart, const char *name)
        {
            const auto it = chart.stage.find(name);
            return it == chart.stage.end() ? nullptr : &it->second;
        }

        // A band member's own texts on its chart track, as GH2's: Rock
        // Band's ways of playing and idling as [play] and [idle], GH2's
        // tempo, wail and face texts as they are, the rest left out.
        std::vector<Chart::Cue> memberCues(const Chart::StageTrack *track)
        {
            static const std::vector<std::string> kPlay = {"[play]", "[mellow]", "[intense]", "[play_solo]"},
                                                  kIdle = {"[idle]", "[idle_realtime]", "[idle_intense]"},
                                                  kKept = {"[half_tempo]", "[normal_tempo]", "[double_tempo]", "[wail_on]",
                                                           "[wail_off]", "[ow_face_on]", "[ow_face_off]"};
            const auto in = [](const std::vector<std::string> &list, const std::string &s)
            { return std::find(list.begin(), list.end(), s) != list.end(); };
            std::vector<Chart::Cue> out;
            for (const Chart::Cue &c : track ? track->cues : std::vector<Chart::Cue>{})
                if (in(kPlay, c.text))
                    out.push_back({c.tick, "[play]"});
                else if (in(kIdle, c.text))
                    out.push_back({c.tick, "[idle]"});
                else if (in(kKept, c.text))
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

        // Where the fret hand sits (notes 40 to 59, the neck's frets 1 to
        // 20), made where a chart has none. A hand on a real neck stays put
        // while its fingers reach the frets near it, so it moves only where
        // a phrase starts (past a beat's rest, a section or a solo's edge),
        // to where that phrase's lowest frets mostly are: green at the
        // third, orange at the twelfth, five higher in a solo. Notes over
        // and over, or trading frets, never move it, and past a rest it
        // waits a bar from its last move, so riffs of short phrases don't
        // twitch it.
        void handPositions(midi::Track &guitar, const Chart &chart, const Difficulty &lead, const TickMap &toGh2)
        {
            static constexpr uint8_t kFret[5] = {3u, 5u, 7u, 9u, 12u};
            const auto inSolo = [&](uint32_t tick)
            {
                return std::any_of(lead.solos.begin(), lead.solos.end(),
                                   [&](const Span &s) { return tick >= s.tick && tick < s.tick + s.length; });
            };
            const auto section = [&](uint32_t tick)
            {
                return std::count_if(chart.sections.begin(), chart.sections.end(),
                                     [&](const auto &s) { return s.first <= tick; });
            };
            std::vector<const Gem *> fretted;
            for (const Gem &g : lead.gems)
                if (g.lanes & kFretLanes)
                    fretted.push_back(&g);
            uint8_t last = 0u;
            uint32_t movedAt = 0u;
            for (size_t i = 0; i < fretted.size();)
            {
                size_t phraseEnd = i + 1u;
                while (phraseEnd < fretted.size() &&
                       fretted[phraseEnd]->tick - fretted[phraseEnd - 1u]->tick <= chart.resolution &&
                       section(fretted[phraseEnd]->tick) == section(fretted[i]->tick) &&
                       inSolo(fretted[phraseEnd]->tick) == inSolo(fretted[i]->tick))
                    ++phraseEnd;
                std::array<int, 5> lowestLaneCount{};
                for (size_t k = i; k < phraseEnd; ++k)
                    ++lowestLaneCount[static_cast<size_t>(
                        std::countr_zero(static_cast<unsigned>(fretted[k]->lanes & kFretLanes)))];
                const size_t lane = static_cast<size_t>(
                    std::max_element(lowestLaneCount.begin(), lowestLaneCount.end()) - lowestLaneCount.begin());
                const uint8_t fret = static_cast<uint8_t>(kFret[lane] + (inSolo(fretted[i]->tick) ? 5u : 0u));
                const uint32_t at = fretted[i]->tick;
                const bool edge = i == 0u || section(at) != section(fretted[i - 1u]->tick) ||
                                  inSolo(at) != inSolo(fretted[i - 1u]->tick);
                if (fret != last && (edge || at - movedAt >= 4u * chart.resolution))
                {
                    addShortNote(guitar, toGh2(at), static_cast<uint8_t>(39u + fret));
                    last = fret;
                    movedAt = at;
                }
                i = phraseEnd;
            }
        }

        // PART BASS as GH2's bassist's strumming hand reads it (bass_strum):
        // the bass part's frets each difficulty, an open on green, and its
        // slap where the chart says.
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

        enum class DrumPattern
        {
            None,
            All,
            Double,
            Half,
            Nobeat,
        };

        const char *beatText(DrumPattern beat)
        {
            return beat == DrumPattern::All ? "[allbeat]" : beat == DrumPattern::Double ? "[double_time]" : beat == DrumPattern::Half ? "[half_time]" : "[nobeat]";
        }

        // A bar's beat by its snares (as GH2's drummer clips: kBandAllbeat,
        // kBandDouble, kBandHalf, kBandNosnare): none, nobeat; one on every
        // beat or two off them, double time; only on the bar's middle beat,
        // half time; else the backbeat.
        DrumPattern barBeat(const std::vector<uint32_t> &snares, uint32_t from, uint32_t to)
        {
            const uint32_t beats = std::max(1u, (to - from + kBeat / 2u) / kBeat);
            std::vector<bool> on(beats, false);
            unsigned offbeats = 0u, count = 0u;
            for (auto it = std::lower_bound(snares.begin(), snares.end(), from); it != snares.end() && *it < to; ++it)
            {
                ++count;
                const uint32_t into = *it - from, beat = (into + kBeat / 8u) / kBeat;
                if (beat < beats && into + kBeat / 8u - beat * kBeat < kBeat / 4u)
                    on[beat] = true;
                else
                    ++offbeats;
            }
            if (count == 0u)
                return DrumPattern::Nobeat;
            if (std::count(on.begin(), on.end(), true) == static_cast<std::ptrdiff_t>(beats) || offbeats >= 2u)
                return DrumPattern::Double;
            if (beats == 4u && on[2] && !on[1] && !on[3])
                return DrumPattern::Half;
            return DrumPattern::All;
        }

        // The drummer's beat bar by bar while the drums play, a change kept
        // only where it holds two bars; [allbeat] at each run's start where
        // there are no snares to say.
        void drumBeats(midi::Track &drums, const std::vector<uint32_t> &bars, const std::vector<uint32_t> &snares,
                       const std::vector<Span> &drumming, bool charted, uint32_t end)
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
                        run.push_back({std::max(bars[b], r.tick), barBeat(snares, bars[b], to)});
                }
                DrumPattern now = DrumPattern::None;
                for (size_t i = 0; i < run.size(); ++i)
                {
                    const DrumPattern beat = run[i].second;
                    const bool holds = i + 1u >= run.size() || run[i + 1u].second == beat;
                    if (beat != now && (now == DrumPattern::None || holds))
                    {
                        drums.events.push_back(midi::text(run[i].first, beatText(beat)));
                        now = beat;
                    }
                }
            }
        }
    }

    std::string sectionBaseName(const std::string &section)
    {
        std::string s;
        for (const unsigned char c : section)
            s += c == '_' || c == '-' ? ' ' : static_cast<char>(std::tolower(c));
        std::vector<std::string> words;
        std::istringstream in(s);
        for (std::string w; in >> w;)
            words.push_back(w);
        // Its number, and a letter or two with it or after it, off the end.
        const auto numbered = [](const std::string &w)
        {
            size_t i = 0;
            while (i < w.size() && std::isdigit(static_cast<unsigned char>(w[i])))
                ++i;
            return (i > 0u && w.size() - i <= 2u) || (w.size() <= 1u);
        };
        while (words.size() > 1u && numbered(words.back()))
            words.pop_back();
        // And one written into its word ("verse1a").
        if (!words.empty())
        {
            std::string &last = words.back();
            size_t letters = last.size();
            while (letters > 0u && last.size() - letters < 2u && std::isalpha(static_cast<unsigned char>(last[letters - 1u])))
                --letters;
            size_t digits = letters;
            while (digits > 0u && std::isdigit(static_cast<unsigned char>(last[digits - 1u])))
                --digits;
            if (digits < letters && digits >= 2u)
                last.resize(digits);
        }
        std::string out;
        for (const std::string &w : words)
            out += (out.empty() ? "" : " ") + w;
        return out;
    }

    std::vector<midi::Track> addStage(const StageIn &in, midi::Track &guitar, midi::Track &events)
    {
        const Chart &chart = in.chart;
        const std::vector<uint32_t> lead = toGh2Sorted(ticksOf(in.lead.gems), in.toGh2);
        if (lead.empty())
            return {};
        const std::vector<Span> playing = playingRuns(lead);
        const Chart::StageTrack *own = stageTrack(chart, trackOf(in.part));
        const std::vector<Chart::Cue> guitarist = memberCues(own);
        addMemberCues(guitar, guitarist, playing, in.toGh2);
        // The fret hand: the chart's places and maps, else made.
        bool placed = false;
        if (own)
        {
            for (const Chart::Mark &m : own->marks)
                if (m.pitch >= 40u && m.pitch <= 59u)
                {
                    const uint32_t from = in.toGh2(m.tick);
                    guitar.events.push_back(midi::note(from, m.pitch, true));
                    guitar.events.push_back(midi::note(std::max(in.toGh2(m.tick + m.length), from + 1u), m.pitch, false));
                    placed = true;
                }
            for (const Chart::Cue &c : own->cues)
                if (const auto map = guitarMap(c.text))
                    guitar.events.push_back(midi::text(in.toGh2(c.tick), *map));
        }
        if (!placed)
            handPositions(guitar, chart, in.lead, in.toGh2);

        // The drums: the chart's where it has them, else the beat while the
        // band plays.
        midi::Track drums;
        drums.name = "BAND DRUMS";
        const std::vector<uint32_t> kicks = chart.kicks.empty() ? beatsIn(playing) : toGh2Sorted(chart.kicks, in.toGh2);
        const std::vector<Span> drumming = chart.drums.empty() ? playing : playingRuns(toGh2Sorted(chart.drums, in.toGh2));
        addMemberCues(drums, memberCues(stageTrack(chart, "PART DRUMS")), drumming, in.toGh2);
        for (const uint32_t t : kicks)
            addShortNote(drums, t, 36u);
        const std::vector<uint32_t> bars = barsTo(chart, in.toGh2, in.end);
        drumBeats(drums, bars, toGh2Sorted(chart.snares, in.toGh2), drumming, !chart.drums.empty(), in.end);

        // The bass: its part's hits, else the kicks.
        midi::Track bass;
        bass.name = "BAND BASS";
        std::vector<uint32_t> pulses = kicks;
        if (const auto it = chart.parts.find(Part::Bass); it != chart.parts.end() && !it->second[3].gems.empty())
            pulses = toGh2Sorted(ticksOf(it->second[3].gems), in.toGh2);
        addMemberCues(bass, memberCues(stageTrack(chart, "PART BASS")), playingRuns(pulses), in.toGh2);
        for (const uint32_t t : pulses)
            addShortNote(bass, t, 36u);

        // The keyboard player, where a song has one (custom/song_files.cpp):
        // where its keys part plays.
        midi::Track keys;
        keys.name = "BAND KEYS";
        if (!chart.keys.empty())
            addMemberCues(keys, memberCues(stageTrack(chart, "PART KEYS")), playingRuns(toGh2Sorted(chart.keys, in.toGh2)), in.toGh2);

        // The singer: the chart's own, else each phrase.
        midi::Track singer;
        singer.name = "BAND SINGER";
        std::vector<Chart::Cue> sings = memberCues(stageTrack(chart, "PART VOCALS"));
        if (!has(sings, "[play]"))
            sings = memberCues(stageTrack(chart, "HARM1"));
        addMemberCues(singer, sings, {}, in.toGh2);
        if (!has(sings, "[play]"))
            for (const Span &s : chart.vocals)
            {
                singer.events.push_back(midi::text(in.toGh2(s.tick), "[play]"));
                singer.events.push_back(midi::text(in.toGh2(s.tick + s.length), "[idle]"));
            }

        // The sections, the chart's or one each eight bars, each with how
        // busy the lead part is in it.
        struct Section
        {
            uint32_t tick;
            std::string look;
            double busy = 0.0;
        };
        std::vector<Section> sections;
        for (const auto &[tick, name] : chart.sections)
            sections.push_back({in.toGh2(tick), sectionBaseName(name)});
        if (sections.empty())
            for (size_t b = 0; b < bars.size(); b += 8u)
                sections.push_back({bars[b], "part " + std::to_string(b / 8u % 4u)});
        std::sort(sections.begin(), sections.end(), [](const Section &a, const Section &b) { return a.tick < b.tick; });
        for (size_t i = 0; i < sections.size(); ++i)
        {
            const uint32_t from = sections[i].tick, to = i + 1u < sections.size() ? sections[i + 1u].tick : in.end;
            const auto n = std::count_if(lead.begin(), lead.end(), [&](uint32_t t) { return t >= from && t < to; });
            sections[i].busy = to > from ? double(n) * kBeat / double(to - from) : 0.0;
        }
        // A look's busyness its sections' mean; the middle of the song's.
        std::map<std::string, std::pair<double, int>> byLook;
        for (const Section &s : sections)
        {
            byLook[s.look].first += s.busy;
            ++byLook[s.look].second;
        }
        std::vector<double> busy;
        for (const Section &s : sections)
            busy.push_back(s.busy);
        std::nth_element(busy.begin(), busy.begin() + static_cast<std::ptrdiff_t>(busy.size() / 2u), busy.end());
        const double middle = busy[busy.size() / 2u];
        const auto lookOf = [&](const std::string &look)
        {
            if (const auto said = named(look))
                return *said;
            const auto &[sum, count] = byLook.at(look);
            return sum / count > middle * kBusier ? Look::Chorus : Look::Verse;
        };
        const auto show = [&](uint32_t tick, Look look, const std::string &name)
        {
            events.events.push_back(midi::text(tick, cue(look)));
            events.events.push_back(midi::text(tick, lighting(look, name)));
        };
        // The guitarist wails through a chorus's first four bars where the
        // chart says nothing of it, as GH2's own charts mostly do.
        const bool wails = !has(guitarist, "[wail_on]");
        for (size_t i = 0; i < sections.size(); ++i)
        {
            const Section &s = sections[i];
            const Look look = lookOf(s.look);
            show(s.tick, look, s.look);
            if (!chart.drums.empty() || look != Look::Verse)
                addShortNote(drums, s.tick, 37u);
            const auto bar = std::lower_bound(bars.begin(), bars.end(), s.tick);
            const uint32_t next = i + 1u < sections.size() ? sections[i + 1u].tick : in.end;
            const uint32_t to = std::min(bars.end() - bar > 4 ? *(bar + 4) : in.end, next);
            const bool played = std::any_of(playing.begin(), playing.end(), [&](const Span &r)
                                            { return s.tick >= r.tick && s.tick < r.tick + r.length; });
            if (wails && look == Look::Chorus && played && to > s.tick)
            {
                guitar.events.push_back(midi::text(s.tick, "[wail_on]"));
                guitar.events.push_back(midi::text(to, "[wail_off]"));
            }
        }
        // The chart's solos a solo's look, then the section's again.
        for (const Span &s : in.lead.solos)
        {
            const uint32_t from = in.toGh2(s.tick), to = in.toGh2(s.tick + s.length);
            show(from, Look::Solo, "solo");
            const auto back = std::find_if(sections.rbegin(), sections.rend(), [&](const Section &x) { return x.tick <= to; });
            if (back != sections.rend())
                show(to, lookOf(back->look), back->look);
        }

        midi::Track triggers;
        triggers.name = "TRIGGERS";
        for (const uint32_t b : bars)
            if (b + kBeat * 4u >= playing.front().tick)
                addShortNote(triggers, b, 48u);

        std::vector<midi::Track> out = {std::move(drums), std::move(bass), std::move(singer), std::move(triggers)};
        if (!keys.events.empty())
            out.push_back(std::move(keys));
        if (midi::Track hand = bassHand(chart, in.toGh2); !hand.events.empty())
            out.push_back(std::move(hand));
        return out;
    }
}
