#include "custom/gh2_chart.h"

#include "formats/midi.h"

#include <algorithm>

namespace gh2::custom
{
    namespace
    {
        // A gem this short is no sustain to GH2 (SongParser, 0x245ff8, under
        // 161 at 480), and a short note is written this long.
        constexpr uint32_t kTap = 60u;

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
        const auto at = [&](uint64_t tick)
        {
            return static_cast<uint32_t>(
                std::min((tick * kGh2Resolution + chart.resolution / 2u) / chart.resolution, kMostTick));
        };

        midi::Track tempo;
        for (const Tempo &t : chart.tempos)
            tempo.events.push_back({at(t.tick), midi::kMeta, midi::kTempo,
                                    {static_cast<uint8_t>(t.usPerBeat >> 16), static_cast<uint8_t>(t.usPerBeat >> 8),
                                     static_cast<uint8_t>(t.usPerBeat)}});
        for (const auto &[tick, sig] : chart.signatures)
            tempo.events.push_back({at(tick), midi::kMeta, midi::kSignature, {sig[0], sig[1], 24u, 8u}});
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
                const uint32_t start = at(g.tick);
                const uint32_t next = i + 1u < gems.size() ? at(gems[i + 1u].tick) : UINT32_MAX;
                uint32_t length = at(uint64_t(g.tick) + g.length) - start;
                length = std::max(length, kTap);
                if (next != UINT32_MAX)
                    length = std::min(length, next - start);
                length = std::max(length, 1u);
                last = std::max(last, start + length);
                for (int lane = 0; lane < 5; ++lane)
                    if (g.lanes & (1u << lane))
                    {
                        guitar.events.push_back(note(start, static_cast<uint8_t>(base + lane), true));
                        guitar.events.push_back(note(start + length, static_cast<uint8_t>(base + lane), false));
                    }
            }
            for (const Span &s : diff.starPower)
            {
                guitar.events.push_back(note(at(s.tick), static_cast<uint8_t>(base + 7u), true));
                guitar.events.push_back(note(std::max(at(uint64_t(s.tick) + s.length), at(s.tick) + 1u), static_cast<uint8_t>(base + 7u), false));
            }
        }
        if (last == 0u)
            return std::nullopt;
        for (const Span &s : lead[3].solos)
        {
            guitar.events.push_back(midi::text(at(s.tick), "[solo_on]"));
            guitar.events.push_back(midi::text(at(uint64_t(s.tick) + s.length), "[solo_off]"));
        }
        order(guitar);

        midi::Track events;
        events.name = "EVENTS";
        // Each name's token by the order names first come.
        std::vector<std::string> names;
        for (const auto &[tick, name] : chart.sections)
        {
            auto it = std::find(names.begin(), names.end(), name);
            if (it == names.end())
                it = names.insert(names.end(), name);
            events.events.push_back(midi::text(at(tick), "[section " + sectionToken(size_t(it - names.begin())) + "]"));
        }
        // The crowd's loops and the world's music_start wait for it
        // (gh1/songs.cpp): two bars in, as GH1's BeatMatch sends it.
        events.events.push_back(midi::text(kGh2Resolution * 8u, "[music_start]"));

        // Two beats past the last note, or the song's stated length.
        midi::File file{1u, static_cast<uint16_t>(kGh2Resolution), {tempo, guitar, events}};
        const midi::TempoMap tempos(file);
        uint32_t end = last + kGh2Resolution * 2u;
        if (lengthSeconds > 0.0)
            end = std::max(end, tempos.tick(lengthSeconds));
        file.tracks[2].events.push_back(midi::text(end, "[end]"));
        order(file.tracks[2]);
        out.mid = midi::write(file);
        return out;
    }
}
