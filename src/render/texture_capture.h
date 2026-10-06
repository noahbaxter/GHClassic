#pragma once

#include "addresses.h"
#include "render/scene.h"

#include <memory>

class PS2Runtime;

namespace gh2
{
    // The pixels a texture object last synced, or null. Game thread only.
    std::shared_ptr<const TextureData> capturedTexture(uint8_t *rdram, uint32_t tex);

    // Before guest memory is replaced with another run's (dev/transplant.h):
    // every texture is then somewhere else, and is decoded when first drawn.
    void forgetTextureAddresses();

    void installTextureCapture(PS2Runtime &runtime, const Addresses &addresses);
}
