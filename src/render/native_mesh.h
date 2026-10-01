#pragma once

#include "addresses.h"
#include "milo/layout.h"
#include "render/scene.h"

#include <cstdint>

class PS2Runtime;

namespace gh2
{
    void installNativeMesh(PS2Runtime &runtime, const Addresses &addresses);

    Matrix toMatrix(const milo::Transform &xfm);

    // The camera current when a draw is made, as an index into the frame
    // being built, reusing the last entry when nothing about it has changed.
    uint32_t currentCamera(uint8_t *rdram);
}
