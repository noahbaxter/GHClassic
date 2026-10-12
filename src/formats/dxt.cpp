#include "formats/dxt.h"

#include <algorithm>

namespace gh2::dxt
{
    namespace
    {
        // A block's bytes in BC's own order.
        struct Block
        {
            const uint8_t *data;

            uint32_t byte(size_t i) const { return data[i ^ 1u]; }
            uint32_t word(size_t i) const { return byte(i) | byte(i + 1u) << 8; }
        };

        void expand565(uint32_t c, uint8_t *out)
        {
            const uint32_t r = c >> 11, g = (c >> 5) & 0x3fu, b = c & 0x1fu;
            out[0] = static_cast<uint8_t>(r << 3 | r >> 2);
            out[1] = static_cast<uint8_t>(g << 2 | g >> 4);
            out[2] = static_cast<uint8_t>(b << 3 | b >> 2);
        }

        // BC1's colour block as 16 RGB triples. With `fourOnly`, BC3's: the
        // two endpoints always span four colours.
        void colours(Block block, bool fourOnly, uint8_t out[16][3])
        {
            const uint32_t c0 = block.word(0u), c1 = block.word(2u);
            uint8_t palette[4][3] = {};
            expand565(c0, palette[0]);
            expand565(c1, palette[1]);
            for (size_t c = 0; c < 3u; ++c)
            {
                const uint32_t a = palette[0][c], b = palette[1][c];
                if (fourOnly || c0 > c1)
                {
                    palette[2][c] = static_cast<uint8_t>((2u * a + b + 1u) / 3u);
                    palette[3][c] = static_cast<uint8_t>((a + 2u * b + 1u) / 3u);
                }
                else
                    palette[2][c] = static_cast<uint8_t>((a + b) / 2u);
            }
            const uint32_t indices = block.word(4u) | block.word(6u) << 16;
            for (size_t i = 0; i < 16u; ++i)
                std::copy_n(palette[(indices >> (2u * i)) & 3u], 3u, out[i]);
        }

        // BC3's alpha block, which BC5 holds two of, as 16 values.
        void alphas(Block block, uint8_t out[16])
        {
            const uint32_t a0 = block.byte(0u), a1 = block.byte(1u);
            uint8_t palette[8];
            palette[0] = static_cast<uint8_t>(a0);
            palette[1] = static_cast<uint8_t>(a1);
            if (a0 > a1)
                for (uint32_t i = 1u; i < 7u; ++i)
                    palette[i + 1u] = static_cast<uint8_t>(((7u - i) * a0 + i * a1 + 3u) / 7u);
            else
            {
                for (uint32_t i = 1u; i < 5u; ++i)
                    palette[i + 1u] = static_cast<uint8_t>(((5u - i) * a0 + i * a1 + 2u) / 5u);
                palette[6] = 0u;
                palette[7] = 255u;
            }
            uint64_t indices = 0u;
            for (size_t i = 0; i < 6u; ++i)
                indices |= static_cast<uint64_t>(block.byte(2u + i)) << (8u * i);
            for (size_t i = 0; i < 16u; ++i)
                out[i] = palette[(indices >> (3u * i)) & 7u];
        }

        size_t blockBytes(Format format) { return format == kDxt1 ? 8u : 16u; }
    }

    size_t levelBytes(Format format, uint32_t width, uint32_t height)
    {
        return static_cast<size_t>(std::max((width + 3u) / 4u, 1u)) * std::max((height + 3u) / 4u, 1u) *
               blockBytes(format);
    }

    bool decode(Format format, const uint8_t *data, size_t size, uint32_t width, uint32_t height,
                std::vector<uint8_t> &rgba)
    {
        if (size < levelBytes(format, width, height))
            return false;
        rgba.assign(static_cast<size_t>(width) * height * 4u, 255u);
        const uint32_t across = std::max((width + 3u) / 4u, 1u);
        for (uint32_t by = 0; by * 4u < height; ++by)
            for (uint32_t bx = 0; bx * 4u < width; ++bx)
            {
                const uint8_t *block = data + (static_cast<size_t>(by) * across + bx) * blockBytes(format);
                uint8_t rgb[16][3], first[16], second[16];
                if (format == kDxt1)
                    colours({block}, false, rgb);
                else
                {
                    alphas({block}, first);
                    if (format == kDxt5)
                        colours({block + 8u}, true, rgb);
                    else
                        alphas({block + 8u}, second);
                }
                for (uint32_t i = 0; i < 16u; ++i)
                {
                    const uint32_t x = bx * 4u + (i & 3u), y = by * 4u + (i >> 2);
                    if (x >= width || y >= height)
                        continue;
                    uint8_t *out = &rgba[(static_cast<size_t>(y) * width + x) * 4u];
                    if (format == kAti2)
                    {
                        out[0] = second[i];
                        out[1] = first[i];
                        continue;
                    }
                    std::copy_n(rgb[i], 3u, out);
                    if (format == kDxt5)
                        out[3] = first[i];
                }
            }
        return true;
    }
}
