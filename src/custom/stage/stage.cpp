#include "custom/stage/parts.h"

#include <algorithm>
#include <cctype>
#include <set>
#include <sstream>

namespace gh2::custom
{
    namespace
    {
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

        // GH2 plays its band's clips and crowd to the chart's beat (main.drv's
        // beat scale). Its own songs play at their own tempo from 88 to 201
        // BPM, Tattooed Love Boys (242) at half, Red Lottery (61) at double
        // ([half_tempo], [double_tempo]). A chart's tempo past these is
        // played at half or double, and back only well inside, so a chart
        // wavering at the line keeps one. A chart charted at twice its
        // song's tempo (or in eighth-note beats) is played at its song's.
        constexpr double kFastest = 210.0, kSlowest = 80.0, kBack = 0.9;

        BandBeatLengths bandBeatLengths(const Chart &chart, const std::function<uint32_t(uint32_t)> &at)
        {
            BandBeatLengths out = {{0u, kBeat}};
            for (const Tempo &t : chart.tempos)
            {
                const double bpm = 60e6 / t.usPerBeat;
                uint32_t beat = out.back().second;
                if (bpm > kFastest)
                    beat = kBeat * 2u;
                else if (bpm < kSlowest)
                    beat = kBeat / 2u;
                else if (bpm < kFastest * kBack && bpm > kSlowest / kBack)
                    beat = kBeat;
                if (beat == out.back().second)
                    continue;
                const uint32_t tick = at(t.tick);
                if (out.back().first == tick)
                    out.back().second = beat;
                else
                    out.push_back({tick, beat});
            }
            return out;
        }

        // The chart's bars as the band's: every other one where it plays at
        // half, each split in two where at double.
        std::vector<uint32_t> bandBars(const std::vector<uint32_t> &bars, const BandBeatLengths &beats, uint32_t end)
        {
            std::vector<uint32_t> out;
            uint32_t regionBeat = UINT32_MAX;
            size_t barInRegion = 0u;
            for (size_t b = 0; b < bars.size(); ++b)
            {
                const uint32_t beat = beatAt(beats, bars[b]);
                if (beat != regionBeat)
                {
                    regionBeat = beat;
                    barInRegion = 0u;
                }
                if (beat > kBeat && barInRegion++ % 2u != 0u)
                    continue;
                out.push_back(bars[b]);
                if (beat < kBeat)
                    out.push_back(bars[b] + ((b + 1u < bars.size() ? bars[b + 1u] : end) - bars[b]) / 2u);
            }
            return out;
        }

        // Where the guitarist's chart track sets no tempo, the band's and
        // crowd's at each change of the band's beat.
        void addBandTempo(BandTracks &band)
        {
            const Chart &chart = band.in.chart;
            const auto authorsTempo = [&](const char *name)
            {
                const Chart::StageTrack *track = stageTrack(chart, name);
                return track && std::any_of(track->cues.begin(), track->cues.end(),
                                            [](const Chart::Cue &c) { return c.text.find("_tempo") != std::string::npos; });
            };
            if (authorsTempo(trackOf(band.in.part)))
                return;
            for (const auto &[tick, beat] : band.beats)
            {
                if (tick == 0u && beat == kBeat)
                    continue;
                const std::string text = beat > kBeat ? "half_tempo" : beat < kBeat ? "double_tempo" : "normal_tempo";
                for (midi::Track *member : {&band.guitar, &band.drums, &band.bass, &band.singer, &band.keys})
                    if (member != &band.keys || !chart.keys.empty())
                        member->events.push_back(midi::text(tick, "[" + text + "]"));
                band.events.events.push_back(midi::text(tick, "[crowd_" + text + "]"));
            }
        }
    }

    std::vector<uint32_t> ticksOf(const std::vector<Gem> &gems)
    {
        std::vector<uint32_t> out;
        for (const Gem &g : gems)
            out.push_back(g.tick);
        return out;
    }

    std::vector<uint32_t> toGh2Sorted(const std::vector<uint32_t> &ticks, const TickMap &toGh2)
    {
        std::vector<uint32_t> out;
        for (const uint32_t t : ticks)
            out.push_back(toGh2(t));
        std::sort(out.begin(), out.end());
        out.erase(std::unique(out.begin(), out.end()), out.end());
        return out;
    }

    const char *trackOf(Part part)
    {
        static constexpr const char *kTracks[] = {"PART GUITAR", "PART GUITAR COOP", "PART RHYTHM", "PART BASS"};
        return kTracks[size_t(part)];
    }

    const Chart::StageTrack *stageTrack(const Chart &chart, const char *name)
    {
        const auto it = chart.stage.find(name);
        return it == chart.stage.end() ? nullptr : &it->second;
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
        const std::vector<uint32_t> lead = toGh2Sorted(ticksOf(in.lead.gems), in.toGh2);
        if (lead.empty())
            return {};
        const BandBeatLengths beats = bandBeatLengths(in.chart, in.toGh2);
        BandTracks band{in, guitar, events, lead, playingRuns(lead), beats,
                        bandBars(barsTo(in.chart, in.toGh2, in.end), beats, in.end)};
        addGuitarist(band);
        const std::vector<uint32_t> kicks = addDrummer(band);
        addBassist(band, kicks);
        addKeys(band);
        addSinger(band);
        const std::vector<Section> sections = sectionsOf(band);
        addMoods(band, sections);
        addSectionCrashes(band, sections);
        addWails(band, sections);
        addKeyframes(band);
        addBandTempo(band);

        std::vector<midi::Track> out;
        for (midi::Track *made : {&band.drums, &band.bass, &band.singer, &band.triggers, &band.keys})
            if (made != &band.keys || !band.keys.events.empty())
                out.push_back(std::move(*made));
        if (midi::Track hand = bassHand(in.chart, in.toGh2); !hand.events.empty())
            out.push_back(std::move(hand));
        return out;
    }
}
