#pragma once

// A custom song's taps drawn as glass under solid tops
// (highway/extended/guitar/tap_gems.cpp).

#include "addresses.h"

#include <cstdint>

class PS2Runtime;

namespace gh2::guitar
{
    // Whether a Gem on the track is a tap, known from when
    // GemManager::AdvanceEnd adds it, before its reps.
    bool isTapGem(uint32_t gem);

    void installTapGems(PS2Runtime &runtime, const Addresses &addresses);
}
