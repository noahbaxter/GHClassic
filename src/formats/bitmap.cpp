#include "formats/bitmap.h"

#include <cstring>

namespace gh2::bitmap
{
    namespace
    {
        // Revision, bits per pixel, encoding, mip count, width, height and
        // row bytes, packed; the rest is zero.
        constexpr size_t kBpp = 1u, kEncoding = 2u, kMips = 6u, kWidth = 7u, kHeight = 9u, kRowBytes = 11u;

        template <typename T>
        T field(const uint8_t *data, size_t at)
        {
            T value;
            std::memcpy(&value, data + at, sizeof(T));
            return value;
        }
    }

    std::optional<Header> read(const uint8_t *data, size_t size)
    {
        if (size < kHeaderBytes || data[0] != 1u)
            return std::nullopt;
        Header header;
        header.bpp = data[kBpp];
        header.encoding = field<uint32_t>(data, kEncoding);
        header.mips = data[kMips];
        header.width = field<uint16_t>(data, kWidth);
        header.height = field<uint16_t>(data, kHeight);
        header.rowBytes = field<uint16_t>(data, kRowBytes);
        return header;
    }

    void write(std::vector<uint8_t> &out, const Header &header)
    {
        const size_t at = out.size();
        out.resize(at + kHeaderBytes, 0u);
        uint8_t *data = out.data() + at;
        data[0] = 1u;
        data[kBpp] = header.bpp;
        std::memcpy(data + kEncoding, &header.encoding, 4u);
        data[kMips] = header.mips;
        std::memcpy(data + kWidth, &header.width, 2u);
        std::memcpy(data + kHeight, &header.height, 2u);
        std::memcpy(data + kRowBytes, &header.rowBytes, 2u);
    }

    size_t paletteBytes(const Header &header)
    {
        return header.bpp <= 8u && (header.encoding & 0x38u) == 0u ? size_t{4u} << header.bpp : 0u;
    }

    size_t paletteSlot(size_t index, uint8_t bpp)
    {
        if (bpp == 8u && (index & 0x18u) == 0x08u)
            return index + 8u;
        if (bpp == 8u && (index & 0x18u) == 0x10u)
            return index - 8u;
        return index;
    }
}
