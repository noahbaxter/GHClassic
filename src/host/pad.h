#pragma once

#include "addresses.h"

#include <cstdint>
#include <string>

class PS2Runtime;

namespace gh2
{
    void installPad(PS2Runtime &runtime, const Addresses &addresses);

    // Buttons a scripted run holds (libpad bits, active high), pressed on top
    // of the host controller's.
    void setScriptedPad(uint16_t pressed);

    // The bit for an action by its name (host/bindings.h), or for up, down,
    // left or right; 0 for anything else.
    uint16_t padButton(const std::string &name);
}
