#pragma once

// A custom chart as GH2's parser takes one (config/midi_parsers.dta): 480
// ticks a beat, PART GUITAR's gems on 60 to 64, 72 to 76, 84 to 88 and 96 to
// 100, each difficulty's star power on the note past its frets (67, 79, 91,
// 103), big notes on 110, [solo_on] and [solo_off], and EVENTS' [section],
// [music_start] and [end].
//
// What GH2's parser cannot say is given beside it (Gh2Chart::gems), each gem's
// kind where the chart decides it or flips the engine's, the song's HOPO
// threshold, and which are opens, for the engine to put in as the gems load
// (gameplay/extended/guitar/gem_kinds.h). Not yet said at all: a chord holds as
// long as its longest lane, and a sustain ends where the next gem starts.

#include "custom/chart.h"

#include <array>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace gh2::custom
{
    constexpr uint32_t kGh2Resolution = 480u;

    // A gem as GH2's parser reads it from the MIDI (its frets), and what the
    // parser cannot read: whether it is strummed or hammered on, and whether
    // it is an open, written as kOpenFrets.
    struct Gh2Gem
    {
        uint8_t frets = 0u; // bit 0 green
        bool hopo = false;
        // Its kind the chart's (forced, or an open), else the running
        // engine's natural one, flipped where `flip`.
        bool decided = false;
        bool flip = false;
        bool open = false;
    };
    constexpr uint8_t kOpenFrets = 0x1fu;

    struct Gh2Chart
    {
        std::vector<uint8_t> mid;
        // Each difficulty's gems by tick, easy first.
        std::array<std::map<uint32_t, Gh2Gem>, 4> gems;
        std::optional<uint32_t> hopoThreshold; // Chart's
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
