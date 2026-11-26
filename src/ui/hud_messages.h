#pragma once

#include "addresses.h"

class PS2Runtime;

namespace gh2
{
    // The HUD's messages one at a time in each place they show, each next
    // one waiting its turn; SONG FAILED first.
    void installHudMessages(PS2Runtime &runtime, const Addresses &addresses);
}
