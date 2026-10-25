#pragma once

#include "addresses.h"

class PS2Runtime;

namespace gh2
{
    // A bar under the rock meter filling as the song plays
    // (ui/dta/song_progress.dta).
    void installSongProgress(PS2Runtime &runtime, const Addresses &addresses);
}
