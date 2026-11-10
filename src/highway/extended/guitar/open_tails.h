#pragma once

// An open's sustain (highway/extended/guitar/open_gems.h) drawn as a wide
// purple tail across the five lanes, its middle see-through and its end round,
// cyan-edged in a star phrase and all cyan under star power, a glow along it
// while it is held, and, whammied, a lane's sustain swelling down its middle.

#include "addresses.h"

#include <cstdint>

class PS2Runtime;
struct R5900Context;

namespace gh2::guitar
{
    // An open rep's tail and glow, if it has them, made an open's.
    void dressOpenTail(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t rep);

    void installOpenTails(PS2Runtime &runtime, const Addresses &addresses);
}
