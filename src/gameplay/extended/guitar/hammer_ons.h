#pragma once

// A custom song's gems (gameplay/extended/guitar/gem_kinds.h) played as Clone
// Hero plays them: an open strummed with no fret held, hit as a HOPO too by
// letting go of the last fret, scored as one fret; a tap hammered on whatever
// is before it; and a HOPO or tap chord hammered on or pulled off to once its
// frets are held, as one. A sustain's frets held are left out
// (gameplay/extended/guitar/sustain_hold.h).

#include "addresses.h"

#include <cstdint>

class PS2Runtime;

namespace gh2::guitar
{
    // The open HOPO (its GameGem) given kOpenLane's fret while it is tried
    // as a hammer-on, 0 when none is.
    uint32_t hammeredOpen();

    // A GameGem's frets: a HOPO or tap chord being hammered on or pulled off
    // to is shown to GH2 as its top fret alone for the while, its own here.
    uint8_t gemFrets(uint8_t *rdram, uint32_t gem);

    void installHammerOns(PS2Runtime &runtime, const Addresses &addresses);
}
