#pragma once

#include "addresses.h"

class PS2Runtime;

namespace gh2
{
    // A crown on each row of the song lists for a song full combo'd on the
    // difficulty being played (save::hasFullCombo): the career's and
    // quickplay's, every game's, and GH1's, which is built from the
    // career's. After ark::install, and before GH1's lists are built from
    // GH2's.
    void installSongCrown(PS2Runtime &runtime, const Addresses &addresses);
}
