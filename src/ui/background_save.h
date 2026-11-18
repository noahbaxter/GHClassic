#pragma once

// A save made with no screen up (campaigns.dta's autosave_goto):
// {ghc_save_start <theme>} as it starts, and the theme's game's
// own memory card icon (formats/ps2_icon.h, off its disc) turning slowly in
// the picture's bottom right corner, fading in and out, up at least three
// seconds from the save's start and as long as the write takes. Its result
// goes to {ghc_saved <result>}: SaveData2 (0x14b5b8) hands it to the UI
// (UIManager::Handle), whose screen is whichever the player is on, so none
// would hear it.

#include "addresses.h"

class PS2Runtime;

namespace gh2::background_save
{
    void install(PS2Runtime &runtime, const Addresses &addresses);
}
