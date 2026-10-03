#pragma once

#include "addresses.h"

class PS2Runtime;

namespace gh2::fast_boot
{
    // Before install: boot no longer holds each logo for three seconds, and
    // goes on to the main menu without the intro movie or splash.
    void enable();

    void install(PS2Runtime &runtime, const Addresses &addresses);
}
