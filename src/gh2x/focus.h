#pragma once

// The 360's depth of field, which its camera shots set and its post stage
// applies (render/frame.h, Frame::depthOfField).
//
// A converted CamShot's keyframe keeps the 360's numbers in the PS2's three
// blur fields, which the game reads only with the shot's own depth of field
// on, and it is off: the blur depth as the near blur, the focus blur
// multiplier as the far blur, and the max blur as the max focal blur plus
// kShotBlurMark, which no PS2 keyframe's reaches. What each keyframe focuses
// on is the same keyframe's target in a second shot under focusShot's name
// (gh2x/classes.h). Both are read as the shot runs.

#include "addresses.h"

#include <string>

class PS2Runtime;

namespace gh2::gh2x
{
    constexpr float kShotBlurMark = 16.0f;
    inline std::string focusShot(const std::string &shot) { return shot + ".focus"; }

    // After the native renderer's own frame hooks, which publish the frame
    // this adds to.
    void installFocus(PS2Runtime &runtime, const Addresses &addresses);
}
