#pragma once

// An open's hit (highway/extended/guitar/open_gems.h) on every fret: each
// smasher pushed up and each lane's burst, in purple, and its held sustain's
// burn one purple flame across the five lanes.

#include "addresses.h"

class PS2Runtime;

namespace gh2::guitar
{
    void installOpenBursts(PS2Runtime &runtime, const Addresses &addresses);
}
