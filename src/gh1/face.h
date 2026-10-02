#pragma once

#include "addresses.h"

class PS2Runtime;

namespace gh2
{
    // Poses GH1 guitarists' faces as GH1's CharFace did, on GH2's RndMorph.
    void installGh1Face(PS2Runtime &runtime, const Addresses &addresses);
}
