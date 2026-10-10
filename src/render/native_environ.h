#pragma once

#include "addresses.h"
#include "render/scene.h"

class PS2Runtime;

namespace gh2
{
    // The environ last selected, which lit draws use until the next. Game
    // thread only.
    const Environ &currentEnviron();

    // What selecting that environ would give, without selecting it. Game
    // thread only; calls into the guest for its lights' transforms.
    Environ readEnviron(uint8_t *rdram, struct R5900Context *ctx, PS2Runtime *runtime, uint32_t env);

    void installNativeEnviron(PS2Runtime &runtime, const Addresses &addresses);
}
