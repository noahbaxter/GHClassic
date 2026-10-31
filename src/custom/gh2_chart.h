#pragma once

// A custom chart as GH2's parser takes one (config/midi_parsers.dta): 480
// ticks a beat, PART GUITAR's gems on 60 to 64, 72 to 76, 84 to 88 and 96 to
// 100, each difficulty's star power on the note past its frets (67, 79, 91,
// 103), big notes on 110, [solo_on] and [solo_off], and EVENTS' [section],
// [music_start] and [end].
//
// What GH2's parser cannot say is given beside it (Gh2Chart::gems), each gem's
// kind where the chart decides it or flips the engine's, the song's HOPO
// threshold, which are opens, a sustain held past the next gem, and a chord's
// lanes ending apart, for the engine to put in as the gems load
// (gameplay/extended/guitar/gem_kinds.h, sustain_hold.h,
// highway/extended/guitar/sustain_tails.h); in the MIDI a sustain ends where
// the next gem starts, and a chord's every lane where its longest does, as
// GH2's parser wants.

#include "custom/chart.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace gh2::custom
{
    constexpr uint32_t kGh2Resolution = 480u;

    // A gem as GH2's parser reads it from the MIDI (its frets), and what the
    // parser cannot read: whether it is strummed or hammered on, whether it
    // is an open, written as kOpenFrets, whether it is a tap (played as a
    // HOPO, drawn as a tap), and a sustain held past the next gem (each lane
    // to the next gem on that lane), its length in ticks and ms, 0 for one
    // as the MIDI has it.
    struct Gh2Gem
    {
        uint8_t frets = 0u; // bit 0 green
        bool hopo = false;
        // Its kind the chart's (forced, a tap or an open), else the running
        // engine's natural one, flipped where `flip`.
        bool decided = false;
        bool flip = false;
        bool open = false;
        bool tap = false;
        uint32_t sustain = 0u;
        float sustainMs = 0.0f;
        // A chord whose lanes end apart: each lane's sustain in ms, by fret
        // (0 for one with none); all 0 for a chord held as one.
        std::array<float, 5> laneMs{};

        bool split() const { return *std::max_element(laneMs.begin(), laneMs.end()) > 0.0f; }
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
