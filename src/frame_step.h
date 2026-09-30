#pragma once

#include "addresses.h"

class PS2Runtime;

namespace gh2
{
    // Keeps the few systems GH2 steps once per frame, rather than by its
    // clocks, at the PS2's rate whatever the frame rate.
    void installFrameStep(PS2Runtime &runtime, const Addresses &addresses);
}
