#pragma once

#include "addresses.h"

class PS2Runtime;

namespace gh2
{
    // Photo mode, a row of the pause menu (dta/photo_mode.dta): the paused
    // song with no track or HUD over it, seen from each of the venue's
    // camera shots in turn or from a camera flown free with W A S D and the
    // mouse. After script::install.
    void installPhotoMode(PS2Runtime &runtime, const Addresses &addresses);
}
