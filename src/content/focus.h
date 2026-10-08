#pragma once

// How far a camera shot's depth of field starts in one of GH1's venues.
//
// GH1 blurs what is behind twice the distance to what the shot looks at,
// measured before the shot's screen spot moves the camera (VenueCam::Poll,
// GH1 0x16ea0c). GH2 measures a held key after that move
// (CamShotFrame::Interp, 0x26696c) and hands PsRnd::SetDepthOfField
// (0x19a798) the distance, the nearest and farthest in focus and an amount,
// of which the PS2 keeps the farthest. A converted shot's amount is how much
// farther the move puts the camera (gh1/cameras.cpp), and the farthest in
// focus is made GH1's from the two. Between two keys Interp blends the
// amount as it does the distance, so while a shot moves, its amount is
// between the 1 of the keys before the last and the last's own, and the
// farthest in focus between twice the distance and GH1's.

#include "addresses.h"

class PS2Runtime;

namespace gh2::focus
{
    void install(PS2Runtime &runtime, const Addresses &addresses);
}
