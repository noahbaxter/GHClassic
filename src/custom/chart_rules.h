#pragma once

// Rules notes_mid.cpp and notes_chart.cpp share.

#include "custom/chart.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace gh2::custom::rules
{
    // A gem as read, before its kind is settled.
    struct Raw
    {
        Gem gem;
        bool hopo = false, strum = false, flip = false, tap = false;

        void add(int fret, uint32_t length)
        {
            gem.lanes |= static_cast<uint8_t>(1u << fret);
            gem.length[fret] = length;
        }
    };

    template <typename T> void sortByTick(std::vector<T> &items)
    {
        std::stable_sort(items.begin(), items.end(), [](const T &a, const T &b) { return a.tick < b.tick; });
    }

    // song.ini's threshold at 480, none where it is GH2's own.
    std::optional<uint32_t> songThreshold(const Options &options, uint32_t resolution);
    // A difficulty's gems from those read by tick: chords made, kinds set by
    // `threshold` (GH2's where none) and the chart's forcing, lane phrases
    // marked. A raw with no fret is dropped.
    void settleGems(Difficulty &d, const std::map<uint32_t, Raw> &raws, uint32_t resolution,
                    std::optional<uint32_t> threshold);
    bool hasGems(const std::array<Difficulty, 4> &part);
    // A section's practice name from an EVENTS text, or none.
    std::optional<std::string> sectionName(const std::string &text);

    // Phrases where a chart marks none: the singing's runs, two beats apart.
    std::vector<Span> phrasesFrom(const std::vector<uint32_t> &ticks, uint32_t resolution);

    // What reading either format ends with: tempos in order, one at 0;
    // sections, phrases and hits in order.
    void finish(Chart &chart);
}
