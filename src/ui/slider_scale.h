#pragma once

// A slider's scale is the volume's, a track with a 0, eleven ticks and an
// 11 drawn in (ui/slider.milo). A slider of anything else is drawn plain:
// the same track and ticks without the digits. Every slider draws from one
// texture, so each is told apart as it draws.
//
//   {slider_scales plain <slider>}   that slider drawn plain
//   {slider_scales colour <slider>}  the colour a plain slider's scale was
//                           last drawn in, for what sits beside it to take
//   {slider_scales clear}   every slider the volume's again: a screen that
//                           made its sliders plain says so as it leaves,
//                           before they go and another's take their place
//                           in memory

#include "addresses.h"

class PS2Runtime;

namespace gh2::slider_scale
{
    void install(PS2Runtime &runtime, const Addresses &addresses);
}
