#pragma once

#include "addresses.h"
#include "render/scene.h"

#include <functional>
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

    // A slider's scale as the volume's is drawn, without its 0 and 11. Null
    // until a slider has loaded.
    std::shared_ptr<const TextureData> plainSliderScale();

    // Every slider's scale captured from here on is that one, or with null
    // the volume's own, and `drawn` is told the material colour (r, g, b) each
    // is drawn in. Game thread only.
    void drawSliderScalesAs(std::shared_ptr<const TextureData> scale,
                            std::function<void(const float *)> drawn = nullptr);

    // A material with that texture is being drawn in that colour.
    void sliderScaleDrawn(uint32_t tex, const float *colour);

    // Before guest memory is replaced with another run's (dev/transplant.h):
    // every texture is then somewhere else, and is decoded when first drawn.
    void forgetTextureAddresses();

    void installTextureCapture(PS2Runtime &runtime, const Addresses &addresses);
}
