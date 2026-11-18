#pragma once

// Up and down held a while on a long list repeat faster.
//
// A held button repeats through its JoypadRepeat: Start (0x2a5eb0) at the
// press, Reset (0x2a5f18) at its release, and Poll (0x2a5f58) each frame
// with the client's hold and repeat times (config's joypad hold_ms and
// repeat_ms), sending a repeat each time the repeat time has passed. While
// a screen asks for it every frame ({list_scroll_fast}, from its panel's
// poll), up and down held there a second repeat at a second, faster pace.
// Every other screen, and a list the moment it stops asking, keeps the
// game's own.

#include "addresses.h"

class PS2Runtime;

namespace gh2::list_scroll
{
    void install(PS2Runtime &runtime, const Addresses &addresses);
}
