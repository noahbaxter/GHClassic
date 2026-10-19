#include "custom/stage/stage.h"

#include "custom/gh2_chart.h"

#include <algorithm>
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

        // [play] a half beat before each run, [idle] a beat past it.
        void playIdle(midi::Track &track, const std::vector<Span> &playing)
        {
            for (const Span &r : playing)
            {
                track.events.push_back(midi::text(r.tick > kBeat / 2u ? r.tick - kBeat / 2u : 0u, "[play]"));
                track.events.push_back(midi::text(r.tick + r.length + kBeat, "[idle]"));
            }
        }

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
        playIdle(guitar, playing);

        // The drums: the chart's where it has them, else the beat while the
        // band plays.
        midi::Track drums;
        drums.name = "BAND DRUMS";
        const std::vector<uint32_t> kicks = chart.kicks.empty() ? beatsIn(playing) : toGh2Sorted(chart.kicks, in.toGh2);
        const std::vector<Span> drumming = chart.drums.empty() ? playing : playingRuns(toGh2Sorted(chart.drums, in.toGh2));
        playIdle(drums, drumming);
        for (const uint32_t t : kicks)
            addShortNote(drums, t, 36u);

        // The bass: its part's hits, else the kicks.
        midi::Track bass;
        bass.name = "BAND BASS";
        std::vector<uint32_t> pulses = kicks;
        if (const auto it = chart.parts.find(Part::Bass); it != chart.parts.end() && !it->second[3].gems.empty())
            pulses = toGh2Sorted(ticksOf(it->second[3].gems), in.toGh2);
        playIdle(bass, playingRuns(pulses));
        for (const uint32_t t : pulses)
            addShortNote(bass, t, 36u);

        midi::Track singer;
        singer.name = "BAND SINGER";
        for (const Span &s : chart.vocals)
        {
            singer.events.push_back(midi::text(in.toGh2(s.tick), "[play]"));
            singer.events.push_back(midi::text(in.toGh2(s.tick + s.length), "[idle]"));
        }

        // The sections, the chart's or one each eight bars, each with how
        // busy the lead part is in it.
        const std::vector<uint32_t> bars = barsTo(chart, in.toGh2, in.end);
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
        for (const Section &s : sections)
        {
            const Look look = lookOf(s.look);
            show(s.tick, look, s.look);
            if (!chart.drums.empty() || look != Look::Verse)
                addShortNote(drums, s.tick, 37u);
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

        return {std::move(drums), std::move(bass), std::move(singer), std::move(triggers)};
    }
}
