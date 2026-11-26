#pragma once

#include "addresses.h"

class PS2Runtime;

namespace gh2
{
    // Face-Off's score meters and messages centred over each player's
    // highway, at 4:3 and in widescreen.
    void installFaceoffHud(PS2Runtime &runtime, const Addresses &addresses);
}
