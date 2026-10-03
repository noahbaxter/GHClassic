#pragma once

#include "addresses.h"

class PS2Runtime;

namespace gh2::card
{
    // The game's memory card, answered here with no files behind it.
    void install(PS2Runtime &runtime, const Addresses &addresses);
}
