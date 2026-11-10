#pragma once

// A custom chart's trill and tremolo lanes (custom/chart.h) drawn as Clone
// Hero and YARG draw them: under each fret's lane notes, from its first to
// its last, a flat see-through band in its colour across its lane (an
// open's purple across all five, less the lane of a fret it trills with),
// its ends round, on the track's surface under all else on it (dividers, beat
// lines, rails, now bar, gems). The band is the tail of the first of them,
// its Gem (GemManager's, the track's own) lengthened to the band's end; its
// GameGem, which the watcher judges, is left as it is, no sustain. Once
// hit, the band scrolls on through the now bar, not held. Judged by
// gameplay/extended/guitar/lane_hits.

#include "addresses.h"

#include <cstdint>

class PS2Runtime;

namespace gh2::guitar
{
    // Whether a tail, by the this PsMesh::Sync takes, is a band's: an
    // open's is laid as one, not pulled apart as its sustain
    // (highway/extended/guitar/open_tails.h).
    bool isBandTail(uint8_t *rdram, uint32_t self);

    void installLaneBands(PS2Runtime &runtime, const Addresses &addresses);
}
