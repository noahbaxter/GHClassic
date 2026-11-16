#pragma once

#include "addresses.h"

class PS2Runtime;

namespace gh2::fast_boot
{
    // Before install: boot no longer holds each logo for three seconds, and
    // goes on to the press-start splash without the intro movie.
    void enable();

    void install(PS2Runtime &runtime, const Addresses &addresses);
}
