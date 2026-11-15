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

    constexpr int kYellowPad = 2, kBluePad = 3, kGreenPad = 4;
    // A pad hit by .chart's lane (notes.mid's 98 to 100 as 2 to 4).
    struct PadHit
    {
        uint32_t tick;
        int lane;
        bool cymbal = true;
    };
    // Of four difficulties' lists, easy first, expert's, else the highest
    // that has any.
    template <typename T> const std::vector<T> &fullest(const std::array<std::vector<T>, 4> &lists)
    {
        size_t d = 3u;
        while (d > 0u && lists[d].empty())
            --d;
        return lists[d];
    }

    // A drum hit by .chart's lane: 0 or 32 the kick, 1 the snare, 2 and up
    // a pad, added to `pads`.
    void addDrum(Chart &chart, std::vector<PadHit> &pads, uint32_t tick, int lane);
    // Crashes from cymbal hits: green, and blue on pro drums only.
    void cymbals(Chart &chart, const std::vector<PadHit> &pads, bool pro);

    // A lyric at `tick` held `length` (0 for no note): markup stripped, a
    // lone '+' extends the syllable before, a repeat at the same tick
    // dropped.
    void addLyric(Chart &chart, uint32_t tick, uint32_t length, const std::string &text);
    // Phrases where a chart marks none: the singing's runs, two beats apart.
    std::vector<Span> phrasesFrom(const std::vector<uint32_t> &ticks, uint32_t resolution);

    // What reading either format ends with: tempos in order, one at 0;
    // signatures, sections, phrases, solos, stage tracks and hits in order;
    // syllables held.
    void finish(Chart &chart);

    // The sections practice picks from: the chart's, else one each tenth
    // of the song from its first note to its last's end, "0%" on. Not the stage's, which
    // makes its own where a chart has none.
    std::vector<std::pair<uint32_t, std::string>> practiceSections(const Chart &chart);
}
