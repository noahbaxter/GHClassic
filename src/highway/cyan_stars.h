#pragma once

// With star_power_overlap (gameplay/engine/star_power_overlap.h), a star
// phrase's gems show it under star power, as cyan stars.

#include "addresses.h"

#include <cstdint>

class PS2Runtime;

namespace gh2::guitar
{
    // Whether a Gem on the track is in a star phrase still to be won, for it
    // to look a phrase's under star power. Always false without
    // star_power_overlap, as such a phrase can't be won then.
    bool inStarPhrase(uint32_t gem);

    void installCyanStars(PS2Runtime &runtime, const Addresses &addresses);
}
