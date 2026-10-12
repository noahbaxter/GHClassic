#pragma once

// The 360's block-compressed textures, decoded. Its blocks are BC1, BC3
// and BC5 with the two bytes of every 16-bit word swapped.

#include <cstddef>
#include <cstdint>
#include <vector>

namespace gh2::dxt
{
    enum Format
    {
        kDxt1, // BC1: colour, 8 bytes a block
        kDxt5, // BC3: alpha then colour, 16 bytes
        kAti2, // BC5: a normal's y then its x, 16 bytes
    };

    // What one level of that size takes.
    size_t levelBytes(Format format, uint32_t width, uint32_t height);

    // One level as RGBA8, width by height. A DXT1 pixel is opaque; an ATI2
    // one is x in red and y in green, blue and alpha 255. False when `size`
    // is short of the level.
    bool decode(Format format, const uint8_t *data, size_t size, uint32_t width, uint32_t height,
                std::vector<uint8_t> &rgba);
}
