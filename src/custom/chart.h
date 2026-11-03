#pragma once

// A custom song's chart, from notes.mid or notes.chart, as far as GH2
// plays one: each difficulty's fretted gems and sustains, star power and
// solos, with its tempos, signatures and sections.

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace gh2::custom
{
    // Every lane struck at one tick.
    struct Gem
    {
        uint32_t tick = 0u;
        uint8_t lanes = 0u; // bit n lane n, green 0 to orange 4
        uint32_t length = 0u; // its longest lane's sustain, 0 none

        bool chord() const { return (lanes & (lanes - 1u)) != 0u; }
    };

    struct Span
    {
        uint32_t tick = 0u, length = 0u;
    };

    struct Difficulty
    {
        std::vector<Gem> gems; // by tick
        std::vector<Span> starPower, solos;
    };

    enum class Part : uint8_t
    {
        Guitar,
        Coop,   // PART GUITAR COOP, .chart's DoubleGuitar
        Rhythm,
        Bass,
    };
    inline constexpr const char *kPartNames[] = {"guitar", "coop", "rhythm", "bass"};
    // Whether `parts`, bit n Part n, has that part.
    inline bool hasPart(uint8_t parts, Part part) { return parts & (1u << int(part)); }

    struct Tempo
    {
        uint32_t tick = 0u, usPerBeat = 500000u;
    };

    struct Chart
    {
        uint32_t resolution = 192u; // ticks a beat
        std::vector<Tempo> tempos;  // by tick, one at 0 at least
        std::vector<std::pair<uint32_t, std::array<uint8_t, 2>>> signatures; // numerator, denominator's log2
        std::vector<std::pair<uint32_t, std::string>> sections; // practice names, "Verse 1"
        std::map<Part, std::array<Difficulty, 4>> parts; // easy first
        // .chart's [Song] Offset, in seconds: the audio's lead.
        double offset = 0.0;

        double seconds(uint32_t tick) const;
    };

    // song.ini's say in how a chart reads.
    struct Options
    {
        // A .mid sustain this short or shorter is none. Unset: a third
        // of a beat for .mid, none for .chart.
        std::optional<uint32_t> sustainCutoff;
        // Star power on note 103 rather than 116 (multiplier_note).
        bool starPowerOn103 = false;
    };

    std::optional<Chart> parseMid(const std::vector<uint8_t> &bytes, const Options &options, std::string &why);
    std::optional<Chart> parseChart(const std::string &text, const Options &options, std::string &why);
    // A song's chart file by its name: a .mid as parseMid reads it, else as
    // parseChart.
    std::optional<Chart> parseSongChart(const std::string &name, const std::string &bytes, const Options &options,
                                        std::string &why);
}
