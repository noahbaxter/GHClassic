#pragma once

// Where a camera shot stands in one of GH1's venues on a wide screen.
//
// A shot holds what it looks at at a spot on screen by moving the camera
// across its view, sideways by the spot's x times the distance over the
// projection's x scale (CamShotFrame::BuildTransform, 0x267008, and Interp,
// 0x2665a0), which is the aspect over tan(yfov / 2). A 16:9 picture's aspect
// is 3/4 of a 4:3 one's, so the camera moves 4/3 as far sideways. GH1 moves
// it as far at any aspect (VenueCam::Poll, GH1 0x16ea3c), and its shots are
// placed for that: at 16:9 the basement's balcony_rt02 stood in a post. So in
// GH1's venues each key's x spot is scaled by the aspect over 4:3's while
// Interp runs, which holds the camera where GH1 does and its view wider.

#include "addresses.h"

class PS2Runtime;

namespace gh2::shot_spot
{
    void install(PS2Runtime &runtime, const Addresses &addresses);
}
