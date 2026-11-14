#pragma once

// Script's {press_holds_size $button}: that BandButton drawn pressed at its
// focused size, not grown, its pressed colour kept.

#include "addresses.h"

class PS2Runtime;

namespace gh2::press_size
{
    void install(PS2Runtime &runtime, const Addresses &addresses);
}
