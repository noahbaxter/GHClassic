#pragma once

// Texture pixels kept on the host, for pictures larger than the guest has
// room for. A payload is where a picture is on a disc. The guest loads a
// stand-in bitmap the size its own texture was, whose first pixel bytes
// name the payload, and the payload is read and decoded when a draw first
// samples it (texture_capture.cpp).

#include "render/scene.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace gh2::textures
{
    // A Tex object of a scene file in a disc's archive, or with no `tex` a
    // bitmap file there.
    struct Source
    {
        size_t disc = 0;
        std::string path;
        std::string tex;
    };

    // Its payload's id, the same for the same source, in any run that
    // mounts the same discs. Nothing is read.
    uint32_t add(const Source &source);

    // The bytes a stand-in's pixels start with: a magic word and the id.
    constexpr size_t kNameBytes = 8u;

    // A bitmap file naming that payload: width by height in the smallest
    // form RndBitmap::Load (0x1b10d0) and PsTex::SyncBitmap take, 4 bits a
    // pixel in the PS2 disc's own order, no mips. Empty when its pixels
    // have no room for the name.
    std::vector<uint8_t> standIn(uint32_t id, uint32_t width, uint32_t height);

    // The payload a bitmap's first pixel bytes name, if it is a stand-in.
    std::optional<uint32_t> named(const uint8_t *pixels, size_t size);

    // Its pixels, every level its disc stores, read now unless something
    // still holds them. Null, with the reason printed, if they cannot be.
    // Game thread only.
    std::shared_ptr<const TextureData> get(uint32_t id);

    // The scene file payloads were last read from is let go.
    void endFrame();
}
