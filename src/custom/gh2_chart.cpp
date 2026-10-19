#include "custom/gh2_chart.h"

#include "custom/stage/stage.h"

#include <algorithm>

namespace gh2::custom
{
    namespace
    {
        // A gem this short is no sustain to either engine (GH1's from 121 at
        // 480, GH2's 161; each cuts its own as the song loads,
        // gameplay/extended/guitar/gem_kinds.h), and a short note is written
        // this long.
        constexpr uint32_t kSustainFrom = 121u, kTap = 60u;
        // A big note (midi_parsers.dta's big_note_parser_guitar).
        constexpr uint8_t kBigNote = 110u;

        using midi::note;

        // Ticks, then a note's end before another's start there.
        void order(midi::Track &track)
        {
            std::stable_sort(track.events.begin(), track.events.end(), [](const midi::Event &a, const midi::Event &b)
                             {
                                 if (a.tick != b.tick)
                                     return a.tick < b.tick;
                                 return midi::kind(a) == midi::kNoteOff && midi::kind(b) != midi::kNoteOff;
                             });
        }
    }

    void addNote(midi::Track &track, uint32_t from, uint32_t to, uint8_t pitch)
    {
        track.events.push_back(note(from, pitch, true));
        track.events.push_back(note(std::max(to, from + 1u), pitch, false));
    }

    std::string sectionToken(size_t n)
    {
        return "custom_section_" + std::to_string(n);
    }

    std::optional<Part> leadPart(uint8_t parts, std::optional<Part> want)
    {
        if (want && hasPart(parts, *want))
            return want;
        for (const Part part : {Part::Guitar, Part::Coop, Part::Rhythm, Part::Bass})
            if (hasPart(parts, part))
                return part;
        return std::nullopt;
    }

    std::optional<Gh2Chart> toGh2(const Chart &chart, double lengthSeconds, std::optional<Part> want)
    {
        Gh2Chart out;
        out.hopoThreshold = chart.hopoThreshold;
        uint8_t parts = 0u;
        for (const auto &[part, difficulties] : chart.parts)
            parts |= static_cast<uint8_t>(1u << int(part));
        const auto played = leadPart(parts, want);
        if (!played)
            return std::nullopt;
        const std::array<Difficulty, 4> &lead = chart.parts.at(*played);
        // Well inside a 4-byte MIDI delta from 0 (0x0fffffff), what a
        // note's end adds after it too, so a chart's wild length still
        // makes a file GH2 and midi::parse read.
        constexpr uint64_t kMostTick = 0x0f000000u;
        const auto gh2Tick = [&](uint64_t tick)
        {
            return static_cast<uint32_t>(
                std::min((tick * kGh2Resolution + chart.resolution / 2u) / chart.resolution, kMostTick));
        };

        midi::Track tempo;
        for (const Tempo &t : chart.tempos)
            tempo.events.push_back({gh2Tick(t.tick), midi::kMeta, midi::kTempo,
                                    {static_cast<uint8_t>(t.usPerBeat >> 16), static_cast<uint8_t>(t.usPerBeat >> 8),
                                     static_cast<uint8_t>(t.usPerBeat)}});
        for (const auto &[tick, sig] : chart.signatures)
            tempo.events.push_back({gh2Tick(tick), midi::kMeta, midi::kSignature, {sig[0], sig[1], 24u, 8u}});
        order(tempo);

        midi::Track guitar;
        guitar.name = "PART GUITAR";
        uint32_t last = 0u;
        for (size_t d = 0; d < 4u; ++d)
        {
            const Difficulty &diff = lead[d];
            const uint8_t base = static_cast<uint8_t>(60u + 12u * d);
            const std::vector<Gem> &gems = diff.gems;
            for (size_t i = 0; i < gems.size(); ++i)
            {
                const Gem &g = gems[i];
                const uint32_t start = gh2Tick(g.tick);
                const uint32_t next = i + 1u < gems.size() ? gh2Tick(gems[i + 1u].tick) : UINT32_MAX;
                uint32_t length = gh2Tick(uint64_t(g.tick) + g.longest()) - start;
                length = std::max(length, kTap);
                if (next != UINT32_MAX)
                    length = std::min(length, next - start);
                length = std::max(length, 1u);
                last = std::max(last, start + length);
                // An open, with no fret of its own, is written as all five
                // frets, told from a chord of all five by Gh2Gem::open. GH2
                // hammers on single frets alone (GuitarTrackWatcherImpl::
                // TryToHopo, 0x24dc20); an open and a chord hammer on as one
                // (gameplay/extended/guitar/hammer_ons.h). An open chord is its
                // frets, a chord to the reader, so never a natural HOPO: its
                // kind the chart's, which GH2's rule, seeing one fret, could
                // not say.
                const bool open = (g.lanes & kFretLanes) == 0u;
                const bool openChord = !open && (g.lanes & (1u << kOpen)) != 0u;
                const uint8_t frets = open ? kOpenFrets : static_cast<uint8_t>(g.lanes & kFretLanes);
                Gh2Gem &kept = out.gems[d][start] = {.frets = frets,
                                                     .hopo = g.kind != Kind::Strum,
                                                     .decided = g.forced || open || openChord,
                                                     .flip = g.flip,
                                                     .open = open,
                                                     .openChord = openChord,
                                                     .tap = g.kind == Kind::Tap,
                                                     .inLanePhrase = g.inLanePhrase,
                                                     .lanePhraseStart = g.lanePhraseStart};
                for (const Difficulty::Band &b : diff.bands)
                    if (b.tick == g.tick)
                    {
                        kept.bandMs[size_t(b.lane)] =
                            static_cast<float>((chart.seconds(b.tick + b.length) - chart.seconds(b.tick)) * 1000.0);
                        if (b.lane == kOpen)
                            for (const Difficulty::Band &other : diff.bands)
                                if (other.lane < kOpen && other.tick <= b.tick + b.length && b.tick <= other.tick + other.length)
                                    kept.openBandGap = static_cast<int8_t>(other.lane);
                    }
                // Each lane held to its own end or the next gem on it, as
                // Clone Hero's; the gem as long as the longest.
                uint32_t held = g.tick;
                std::array<uint32_t, 5> ends{};
                for (int lane = 0; lane <= kOpen; ++lane)
                    if (g.lanes & (1u << lane))
                    {
                        uint32_t end = g.tick + g.length[size_t(lane)];
                        for (size_t k = i + 1u; k < gems.size() && gems[k].tick < end; ++k)
                            if (gems[k].lanes & (1u << lane))
                                end = gems[k].tick;
                        held = std::max(held, end);
                        if (lane < 5)
                            ends[size_t(lane)] = end;
                    }
                // The song ends past its sustains too, a held one past the
                // last gem's end.
                last = std::max(last, gh2Tick(held));
                if (gh2Tick(held) - start > length && gh2Tick(held) - start >= kSustainFrom)
                {
                    kept.sustain = gh2Tick(held) - start;
                    kept.sustainMs = static_cast<float>((chart.seconds(held) - chart.seconds(g.tick)) * 1000.0);
                }
                // A chord whose lanes end apart, each lane's.
                uint32_t shortest = held;
                for (size_t lane = 0; lane < 5u; ++lane)
                    if (frets & (1u << lane))
                        shortest = std::min(shortest, ends[lane]);
                if (g.chord() && !open && gh2Tick(shortest) < gh2Tick(held) && gh2Tick(held) - start >= kSustainFrom)
                    for (size_t lane = 0; lane < 5u; ++lane)
                        if (frets & (1u << lane))
                            kept.laneMs[lane] =
                                static_cast<float>((chart.seconds(ends[lane]) - chart.seconds(g.tick)) * 1000.0);
                for (int lane = 0; lane < 5; ++lane)
                    if (frets & (1u << lane))
                        addNote(guitar, start, start + length, static_cast<uint8_t>(base + lane));
            }
            for (const Span &s : diff.starPower)
                addNote(guitar, gh2Tick(s.tick), gh2Tick(uint64_t(s.tick) + s.length), static_cast<uint8_t>(base + 7u));
            // Each player's Face-Off phrases on the two notes past it: GH2's
            // parser gives a gem started under one to that player alone, one
            // under neither to both (SongParser::SetActivePlayer, 0x246928).
            for (uint8_t player = 0u; player < 2u; ++player)
                for (const Span &s : diff.faceOff[player])
                    addNote(guitar, gh2Tick(s.tick), gh2Tick(uint64_t(s.tick) + s.length), static_cast<uint8_t>(base + 9u + player));
        }
        if (last == 0u)
            return std::nullopt;
        for (const Span &s : lead[3].solos)
        {
            guitar.events.push_back(midi::text(gh2Tick(s.tick), "[solo_on]"));
            guitar.events.push_back(midi::text(gh2Tick(uint64_t(s.tick) + s.length), "[solo_off]"));
        }
        for (const Span &s : lead[3].bigNotes)
            addNote(guitar, gh2Tick(s.tick), gh2Tick(uint64_t(s.tick) + s.length), kBigNote);

        midi::Track events;
        events.name = "EVENTS";
        // Each name's token by the order names first come.
        std::vector<std::string> names;
        for (const auto &[tick, name] : chart.sections)
        {
            auto it = std::find(names.begin(), names.end(), name);
            if (it == names.end())
                it = names.insert(names.end(), name);
            events.events.push_back(midi::text(gh2Tick(tick), "[section " + sectionToken(size_t(it - names.begin())) + "]"));
        }
        // The crowd's loops and the world's music_start wait for it
        // (gh1/songs.cpp): two bars in, as GH1's BeatMatch sends it.
        events.events.push_back(midi::text(kGh2Resolution * 8u, "[music_start]"));

        // Two beats past the last note, or the song's stated length.
        const midi::TempoMap tempos(midi::File{1u, static_cast<uint16_t>(kGh2Resolution), {tempo}});
        uint32_t end = last + kGh2Resolution * 2u;
        if (lengthSeconds > 0.0)
            end = std::max(end, tempos.tick(lengthSeconds));
        events.events.push_back(midi::text(end, "[end]"));

        std::vector<midi::Track> band = addStage({chart, lead[3], gh2Tick, end}, guitar, events);
        midi::File file{1u, static_cast<uint16_t>(kGh2Resolution), {tempo, guitar, events}};
        for (midi::Track &track : band)
            file.tracks.push_back(std::move(track));
        for (size_t t = 1u; t < file.tracks.size(); ++t)
            order(file.tracks[t]);
        out.mid = midi::write(file);
        return out;
    }
}
