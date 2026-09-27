#pragma once

#include "addresses.h"
#include "render/scene.h"

class PS2Runtime;

namespace gh2
{
    // The material a pass draws with, read from a RndMat. Game thread only.
    Material readMaterial(uint8_t *rdram, uint32_t mat);

    // The pass after `mat`, or 0.
    uint32_t nextPass(uint8_t *rdram, uint32_t mat);

    void installNativeMat(PS2Runtime &runtime, const Addresses &addresses);
}
