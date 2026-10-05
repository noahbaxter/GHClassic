#pragma once

// Keys for testing, off unless --cheats is given, which also keeps saves
// off the disk (main.cpp):
//
//   F3, F4, F5   in a song, win it now with that many stars, as the game's
//                own debug keys do ({player0 win <stars>}, cheats.dta)

#include "addresses.h"

#include <cstdint>

class PS2Runtime;

namespace gh2::cheats
{
    void enable();
    // A key pressed, by its SDL keycode. Any thread.
    void key(uint32_t keycode);

    void install(PS2Runtime &runtime, const Addresses &addresses);
}
