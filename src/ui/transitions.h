#pragma once

#include "addresses.h"

class PS2Runtime;

namespace gh2
{
    // With snappy_menus: a screen slides out and the next in over a sixth of
    // a second each, not half.
    void installTransitions(PS2Runtime &runtime, const Addresses &addresses);
}
