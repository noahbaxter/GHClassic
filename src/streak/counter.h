#pragma once

#include "addresses.h"

class PS2Runtime;

namespace gh2
{
    // A note streak counter under the score meter (ui/dta/streak_counter.dta).
    // After ark::install.
    void installStreakCounter(PS2Runtime &runtime, const Addresses &addresses);
}
