// A song's chart by its file's kind: notes.mid read in notes_mid.cpp,
// notes.chart in notes_chart.cpp, each by the rules of chart_rules.cpp.

#include "custom/chart.h"

#include "custom/text.h"

#include <algorithm>
#include <filesystem>

namespace gh2::custom
{
    bool Gem::split() const
    {
        for (int i = 0; i <= kOpen; ++i)
            if ((lanes & (1u << i)) && length[i] != longest())
                return true;
        return false;
    }

    uint32_t Gem::longest() const { return *std::max_element(length.begin(), length.end()); }

    std::vector<Span> runs(std::vector<uint32_t> ticks, uint32_t gap, uint32_t tail)
    {
        std::sort(ticks.begin(), ticks.end());
        std::vector<Span> out;
        for (const uint32_t t : ticks)
            if (!out.empty() && t <= out.back().tick + out.back().length + gap)
                out.back().length = std::max(out.back().length, t + tail - out.back().tick);
            else
                out.push_back({t, tail});
        return out;
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

    std::optional<Chart> parseSongChart(const std::string &name, const std::string &bytes, const Options &options,
                                        std::string &why)
    {
        if (lower(std::filesystem::path(name).extension().string()) == ".mid")
            return parseMid({bytes.begin(), bytes.end()}, options, why);
        return parseChart(bytes, options, why);
    }
}
