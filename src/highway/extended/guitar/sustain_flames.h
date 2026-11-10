#pragma once

// A custom song's burn flames, one a fret held for a sustain
// (gameplay/extended/guitar/sustain_hold.h): each burning on through later hits
// and out at its own sustain's end.

#include "addresses.h"

class PS2Runtime;

namespace gh2::guitar
{
    void installSustainFlames(PS2Runtime &runtime, const Addresses &addresses);
}
