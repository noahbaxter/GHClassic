#pragma once

// An open (gameplay/extended/guitar/gem_kinds.h), a gem with no fret, drawn as
// one wide purple gem across all five lanes, as GH2 Deluxe draws a kick: GH2's
// own gem pulled apart at its middle to span the lanes and flattened, its blue
// lane's on gem.tex turned purple. Its pieces are added to the track's scene; a
// gem with no fret is drawn on the middle lane, each of its parts as the wide
// one of the same name (open_<part>.mesh). An open chord
// (gameplay/extended/guitar/open_chord_hits.h) has one too, its frets' gems
// lifted onto it as a drum chord's pads stand over its kick, their sustains
// left on the track over the open's.

#include "addresses.h"

#include <cstdint>

class PS2Runtime;

namespace gh2::guitar
{
    // Whether a GemRep is an open's.
    bool isOpenRep(uint32_t rep);
    // Whether it is an open chord's (its frets lifted onto it).
    bool isOpenChordRep(uint32_t rep);
    // The track's scene (its ObjectDir) an open's pieces were found in, 0
    // before one is drawn.
    uint32_t openScene();

    void installOpenGems(PS2Runtime &runtime, const Addresses &addresses);
}
