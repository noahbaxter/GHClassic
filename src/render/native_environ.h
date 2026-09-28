#pragma once

#include "addresses.h"
#include "render/scene.h"

class PS2Runtime;

namespace gh2
{
    // The environ last selected, which lit draws use until the next. Game
    // thread only.
    const Environ &currentEnviron();

    void installNativeEnviron(PS2Runtime &runtime, const Addresses &addresses);
}
