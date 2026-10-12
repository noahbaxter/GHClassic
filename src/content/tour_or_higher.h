#pragma once

// A reward earned on its difficulty or any above it. The disc's config gates
// a reward on one difficulty's tour, `(require tour_passed) (difficulty
// kDifficultyHard)`, and the tour of a harder one does not count. A
// difficulty with kOrHigher added counts them: Campaign::AllSongsPassed and
// AllSongsFiveStars are asked that difficulty and each above it.

#include "addresses.h"

#include <cstdint>

class PS2Runtime;

namespace gh2
{
    constexpr int32_t kOrHigher = 0x100;

    void installTourOrHigher(PS2Runtime &runtime, const Addresses &addresses);
}
