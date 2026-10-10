#pragma once

// The 360's character shadow maps (render/frame.h, Frame::shadows).
//
// The PS2's Character draws one too where the graphics mode is the new one,
// which it never is on a PS2 (Character::DrawShowing 0x161c68: at its highest
// LOD, drawn the usual way, with self_shadow set). DrawShadowBuffer
// (0x1619c0) aims a camera from the environ's first directional light at the
// character's world sphere and draws the character through it, and
// DrawShowing gives the result to Rnd::SetShadowMap for the character's own
// draw and takes it back after. With x360 shading DrawShadowBuffer is
// replaced by one that makes that camera for the host and has the game draw
// the character's meshes into it, and SetShadowMap, empty on the PS2, says
// which draws a map is for.

#include "addresses.h"

class PS2Runtime;

namespace gh2::gh2x
{
    void installShadows(PS2Runtime &runtime, const Addresses &addresses);
}
