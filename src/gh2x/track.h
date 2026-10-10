#pragma once

// The 360's look on the PS2's track.
//
// The track's scene is the PS2's with the 360's Mats, pictures, lights and
// environs in it (gh2x/scene.h, convertTrack). Two things the scene cannot
// say are told to the renderer from here. The board is a Mat the track's
// code makes and puts the guitarist's fretboard on
// (CharTrackSurface::SetCharTexture, 0x155470), with no name, so which Mat
// it is has to be caught as it is set. And the 360 draws the board under an
// environ of its own, two directional lights and full ambient, where the
// PS2's scene draws all of the track under one (render/look.h).

#include "addresses.h"

class PS2Runtime;

namespace gh2::gh2x
{
    void installTrack(PS2Runtime &runtime, const Addresses &addresses);
}
