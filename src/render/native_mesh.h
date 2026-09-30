#pragma once

#include "addresses.h"
#include "render/scene.h"

#include <cstdint>

class PS2Runtime;

namespace gh2
{
    void installNativeMesh(PS2Runtime &runtime, const Addresses &addresses);

    // A Transform in guest memory: three 16-byte rows, then the position.
    Matrix readTransform(uint8_t *rdram, uint32_t address);

    // The camera current when a draw is made, as an index into the frame
    // being built, reusing the last entry when nothing about it has changed.
    uint32_t currentCamera(uint8_t *rdram);
}
