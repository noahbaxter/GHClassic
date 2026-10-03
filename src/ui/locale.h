#pragma once

#include "addresses.h"

class PS2Runtime;

namespace gh2
{
    // The port's own text for the game's locale tokens (ui/dta/locale.dta),
    // in place of the disc's.
    void installLocale(PS2Runtime &runtime, const Addresses &addresses);
}
