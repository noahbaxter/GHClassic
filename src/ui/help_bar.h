#pragma once

#include "addresses.h"

class PS2Runtime;

namespace gh2
{
    // The help bar with all five frets: (fret4 <token>) and (fret5 <token>)
    // show blue and orange, and yellow gets icon art like green's and
    // red's. After ark::install.
    void installHelpBar(PS2Runtime &runtime, const Addresses &addresses);
}
