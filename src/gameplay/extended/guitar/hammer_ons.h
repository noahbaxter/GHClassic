#pragma once

// A custom song's gems (gameplay/extended/guitar/gem_kinds.h) played as Clone
// Hero plays them: an open strummed with no fret held, hit as a HOPO too by
// letting go of the last fret, scored as one fret; a tap hammered on whatever
// is before it; and a HOPO or tap chord hammered on once its frets are held, as
// one.

#include "addresses.h"

#include <cstdint>

class PS2Runtime;

namespace gh2::guitar
{
    // The open HOPO (its GameGem) given kOpenLane's fret while it is tried
    // as a hammer-on, 0 when none is.
    uint32_t hammeredOpen();

    void installHammerOns(PS2Runtime &runtime, const Addresses &addresses);
}
