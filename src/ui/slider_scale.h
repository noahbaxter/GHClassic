#pragma once

// A slider's scale is the volume's, a track with a 0, eleven ticks and an
// 11 drawn in (ui/slider.milo). A slider of anything else is drawn plain:
// the same track and ticks without the digits. Every slider draws from one
// texture, so each is told apart as it draws.
//
//   {slider_scales plain <slider>}   that slider drawn plain
//   {slider_scales colour <slider>}  the colour a plain slider's scale was
//                           last drawn in, for what sits beside it to take
//   {slider_scales off <slider or box> <colour>}  a plain slider's knob and
//                           scale, or a tick box, drawn in that colour, as
//                           one that is off: neither has a look of its own
//                           for that. {slider_scales on ...} is as it was
//
// Each lasts as long as its slider or box does.

#include "addresses.h"

class PS2Runtime;

namespace gh2::slider_scale
{
    void install(PS2Runtime &runtime, const Addresses &addresses);
}
