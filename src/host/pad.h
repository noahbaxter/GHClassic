#pragma once

#include "addresses.h"

#include <cstdint>

class PS2Runtime;

namespace gh2
{
    void installPad(PS2Runtime &runtime, const Addresses &addresses);

    // Buttons a scripted run holds (libpad bits, active high), pressed on top
    // of the host controller's.
    void setScriptedPad(uint16_t pressed);
}
