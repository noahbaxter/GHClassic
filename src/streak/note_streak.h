#pragma once

#include "addresses.h"

class PS2Runtime;

namespace gh2
{
    // Before locale::install.
    void installNoteStreak(PS2Runtime &runtime, const Addresses &addresses);
}
