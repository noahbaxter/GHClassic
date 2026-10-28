#pragma once

#include "addresses.h"

#include <cstdint>
#include <string>

class PS2Runtime;

namespace gh2
{
    void installPad(PS2Runtime &runtime, const Addresses &addresses);

    // Buttons a scripted run holds on P1's port (libpad bits, active high),
    // pressed on top of its seat's. Another seat's are a scenario's device
    // in it (input.h).
    void setScriptedPad(uint16_t pressed);

    // The bit for an action by its name (host/bindings.h), or for up, down,
    // left or right; 0 for anything else.
    uint16_t padButton(const std::string &name);
}
