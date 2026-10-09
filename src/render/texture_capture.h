#pragma once

#include "addresses.h"
#include "render/scene.h"

#include <memory>
#include <optional>

class PS2Runtime;

namespace gh2
{
    // The pixels a texture object last synced, or null. Game thread only.
    std::shared_ptr<const TextureData> capturedTexture(uint8_t *rdram, uint32_t tex);

    // The host payload a texture object's bitmap stands in for
    // (render/texture_store.h), if it is a stand-in. Game thread only.
    std::optional<uint32_t> standInPayload(uint32_t tex);

    // Before guest memory is replaced with another run's (dev/transplant.h):
    // every texture is then somewhere else, and is decoded when first drawn.
    void forgetTextureAddresses();

    void installTextureCapture(PS2Runtime &runtime, const Addresses &addresses);
}
