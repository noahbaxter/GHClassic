#pragma once

#include "addresses.h"
#include "render/scene.h"

#include <memory>

class PS2Runtime;

namespace gh2
{
    // The pixels a texture object last synced, or null. Game thread only.
    std::shared_ptr<const TextureData> capturedTexture(uint32_t tex);

    void installTextureCapture(PS2Runtime &runtime, const Addresses &addresses);
}
