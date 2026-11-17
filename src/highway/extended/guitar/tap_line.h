#pragma once

// A line along a custom song's run of taps, from each to the next, as later
// games draw it (highway/extended/guitar/tap_line.cpp).

#include "addresses.h"

class PS2Runtime;

namespace gh2::guitar
{
    void installTapLine(PS2Runtime &runtime, const Addresses &addresses);
}
