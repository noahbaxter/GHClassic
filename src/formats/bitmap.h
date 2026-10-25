#pragma once

// A Harmonix bitmap's header: what a Tex object embeds and a .bmp_ps2 or
// .png_xbox file starts with, revision 1 as RndBitmap::LoadHeader (0x1adeb8)
// reads it. A PS2's is followed by a palette and pixels, a 360's by
// block-compressed levels (formats/dxt.h).

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace gh2::bitmap
{
    constexpr size_t kHeaderBytes = 32u;

    // The 360's encodings. A PS2's is RndBitmap's order bits (milo/layout.h).
    enum Encoding : uint32_t
    {
        kDxt1 = 8u,
        kDxt5 = 24u,
        kAti2 = 32u,
    };

    struct Header
    {
        uint8_t bpp = 0;
        uint32_t encoding = 0;
        uint8_t mips = 0; // levels after the first
        uint16_t width = 0;
        uint16_t height = 0;
        uint16_t rowBytes = 0;
    };

    // None when `size` is short of a header or its revision is not 1.
    std::optional<Header> read(const uint8_t *data, size_t size);

    void write(std::vector<uint8_t> &out, const Header &header);

    // RndBitmap::PaletteBytes (0x1ae3e8): 4 bytes an entry up to 8 bits a
    // pixel, none for a block-compressed one.
    size_t paletteBytes(const Header &header);

    // Where a palette's entry for an index is: an 8-bit one is stored in the
    // GS's CSM1 order.
    size_t paletteSlot(size_t index, uint8_t bpp);
}
