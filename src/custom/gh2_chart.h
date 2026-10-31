#pragma once

// A custom chart as GH2's parser takes one (config/midi_parsers.dta): 480
// ticks a beat, PART GUITAR's gems on 60 to 64, 72 to 76, 84 to 88 and 96 to
// 100, each difficulty's star power on the note past its frets (67, 79, 91,
// 103), [solo_on] and [solo_off], and EVENTS' [section], [music_start] and
// [end]. Every gem's kind is GH2's natural one, and a sustain ends where the
// next gem starts.

#include "custom/chart.h"

#include <cstdint>
#include <string>
#include <vector>

namespace gh2::custom
{
    constexpr uint32_t kGh2Resolution = 480u;

    struct Gh2Chart
    {
        std::vector<uint8_t> mid;
    };

    // The part PART GUITAR is of a chart with `parts` (a bit each Part):
    // `want` where it has it, else its guitar, co-op, rhythm or bass part,
    // the first it has.
    std::optional<Part> leadPart(uint8_t parts, std::optional<Part> want = {});

    // `lengthSeconds`, song.ini's song_length, ends it no sooner than that;
    // PART GUITAR is leadPart's.
    std::optional<Gh2Chart> toGh2(const Chart &chart, double lengthSeconds, std::optional<Part> want = {});

    // The locale token of a chart's nth section by name, in the order each
    // first comes, as Song::sections has them: custom_section_<n>.
    std::string sectionToken(size_t n);
}
