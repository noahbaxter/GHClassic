// The venue's show, as stage.h has it: each section's mood and lighting
// on EVENTS, the lighting's keyframes on TRIGGERS.

#include "custom/stage/parts.h"

#include <algorithm>
#include <optional>

namespace gh2::custom
{
    namespace
    {
        // A section whose name says nothing is a chorus when this much busier
        // than the median section.
        constexpr double kBusier = 1.2;

        uint32_t nameHash(const std::string &s)
        {
            uint32_t h = 2166136261u;
            for (const unsigned char c : s)
                h = (h ^ c) * 16777619u;
            return h;
        }

        // The mood a section's name says, if any.
        std::optional<Mood> moodFromName(const std::string &name)
        {
            if (name.find("solo") != std::string::npos)
                return Mood::Solo;
            for (const char *word : {"chorus", "refrain", "hook"})
                if (name.find(word) != std::string::npos)
                    return Mood::Chorus;
            for (const char *word : {"verse", "intro", "outro", "bridge", "break", "interlude", "pre"})
                if (name.find(word) != std::string::npos)
                    return Mood::Verse;
            return std::nullopt;
        }

        // A section's [lighting (...)] for its mood, the same for each
        // section of its name.
        std::string lighting(Mood mood, const std::string &name)
        {
            static const std::vector<std::string> kLights[] = {{"()", "(color1)", "(sweep)"}, {"(color2)", "(chase)"}, {"(strobe)"}};
            const auto &from = kLights[size_t(mood)];
            return "[lighting " + from[nameHash(name) % from.size()] + "]";
        }

        constexpr const char *kMoodTexts[] = {"[verse]", "[chorus]", "[solo]"}; // by Mood

    }

    std::vector<Section> sectionsOf(const BandTracks &band)
    {
        std::vector<Section> sections;
        for (const auto &[tick, name] : band.in.chart.sections)
            sections.push_back({band.in.toGh2(tick), sectionBaseName(name)});
        if (sections.empty())
            for (size_t b = 0; b < band.bars.size(); b += 8u)
                sections.push_back({band.bars[b], "part " + std::to_string(b / 8u % 4u)});
        std::sort(sections.begin(), sections.end(), [](const Section &a, const Section &b) { return a.tick < b.tick; });
        for (size_t i = 0; i < sections.size(); ++i)
        {
            const uint32_t from = sections[i].tick, to = i + 1u < sections.size() ? sections[i + 1u].tick : band.in.end;
            const auto n = std::count_if(band.lead.begin(), band.lead.end(), [&](uint32_t t) { return t >= from && t < to; });
            sections[i].busy = to > from ? double(n) * kBeat / double(to - from) : 0.0;
        }
        std::map<std::string, std::pair<double, int>> byName;
        std::vector<double> busy;
        for (const Section &s : sections)
        {
            auto &[sum, n] = byName[s.name];
            sum += s.busy;
            ++n;
            busy.push_back(s.busy);
        }
        std::nth_element(busy.begin(), busy.begin() + static_cast<std::ptrdiff_t>(busy.size() / 2u), busy.end());
        const double median = busy[busy.size() / 2u];
        for (Section &s : sections)
            if (const auto named = moodFromName(s.name))
                s.mood = *named;
            else
            {
                const auto &[sum, count] = byName.at(s.name);
                s.mood = sum / count > median * kBusier ? Mood::Chorus : Mood::Verse;
            }
        return sections;
    }

    void addMoods(BandTracks &band, const std::vector<Section> &sections)
    {
        const auto addMood = [&](uint32_t tick, Mood mood, const std::string &name)
        {
            band.events.events.push_back(midi::text(tick, kMoodTexts[size_t(mood)]));
            band.events.events.push_back(midi::text(tick, lighting(mood, name)));
        };
        for (const Section &s : sections)
            addMood(s.tick, s.mood, s.name);
        // Each solo the solo mood, then its section's again at its end.
        for (const Span &s : band.in.lead.solos)
        {
            const uint32_t from = band.in.toGh2(s.tick), to = band.in.toGh2(s.tick + s.length);
            addMood(from, Mood::Solo, "solo");
            const auto back = std::find_if(sections.rbegin(), sections.rend(), [&](const Section &x) { return x.tick <= to; });
            if (back != sections.rend())
                addMood(to, back->mood, back->name);
        }
    }

    void addKeyframes(BandTracks &band)
    {
        band.triggers.name = "TRIGGERS";
        for (const uint32_t b : band.bars)
            if (b + kBeat * 4u >= band.playing.front().tick)
                addShortNote(band.triggers, b, 48u);
    }
}
