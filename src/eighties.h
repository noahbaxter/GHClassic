#pragma once

#include "addresses.h"

class PS2Runtime;

namespace gh2
{
    // The 80s guitarists as extra outfits, when the 80s disc is mounted.
    // After ark::install, which indexes the discs.
    void installEighties(PS2Runtime &runtime, const Addresses &addresses);
}
