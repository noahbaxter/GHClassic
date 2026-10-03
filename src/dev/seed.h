#pragma once

#include "addresses.h"

class PS2Runtime;

namespace gh2::seed
{
    // Before install: the game's random numbers start from this seed in
    // place of the time of day, so a run plays out the same every time.
    void fix(int seed);

    void install(PS2Runtime &runtime, const Addresses &addresses);
}
