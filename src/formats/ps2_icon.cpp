#include "formats/ps2_icon.h"

#include <cstring>

namespace gh2::ps2_icon
{
    namespace
    {
        constexpr uint32_t kMagic = 0x00010000u;
        constexpr size_t kHeader = 20u;
        constexpr size_t kTextureBytes = kTextureSide * kTextureSide * 2u;

        template <typename T>
        bool read(const std::vector<uint8_t> &file, size_t at, T &out)
        {
            if (at + sizeof(T) > file.size())
                return false;
            std::memcpy(&out, file.data() + at, sizeof(T));
            return true;
        }

        // 1-5-5-5, red lowest.
        void putColor(std::vector<uint8_t> &rgba, uint16_t c)
        {
            rgba.push_back(static_cast<uint8_t>((c & 0x1fu) << 3));
            rgba.push_back(static_cast<uint8_t>(((c >> 5) & 0x1fu) << 3));
            rgba.push_back(static_cast<uint8_t>(((c >> 10) & 0x1fu) << 3));
            rgba.push_back(255u);
        }

        // Where the picture starts past the animation, its frames' keys
        // counted `less` short of their count, or 0 if it runs past the file.
        size_t pastAnimation(const std::vector<uint8_t> &file, size_t at, uint32_t frames, uint32_t less)
        {
            for (uint32_t f = 0; f < frames; ++f)
            {
                uint32_t keys = 0;
                if (!read(file, at + 4u, keys) || keys < less)
                    return 0u;
                at += 16u + (static_cast<size_t>(keys) - less) * 8u;
                if (at > file.size())
                    return 0u;
            }
            return at;
        }

        // The picture at `at`, plain or run-length coded (a count under
        // 0xff00 repeats the next word that many times, one above copies
        // 0x10000 less it words), if it ends the file.
        bool picture(const std::vector<uint8_t> &file, size_t at, std::vector<uint8_t> &rgba)
        {
            rgba.clear();
            if (file.size() - at == kTextureBytes)
            {
                for (size_t i = 0; i < kTextureBytes; i += 2u)
                {
                    uint16_t c = 0;
                    read(file, at + i, c);
                    putColor(rgba, c);
                }
                return true;
            }
            uint32_t size = 0;
            if (!read(file, at, size) || at + 4u + size != file.size())
                return false;
            const size_t pixels = kTextureSide * kTextureSide;
            for (size_t p = at + 4u; p + 2u <= file.size() && rgba.size() < pixels * 4u;)
            {
                uint16_t code = 0;
                read(file, p, code);
                p += 2u;
                if (code < 0xff00u)
                {
                    uint16_t c = 0;
                    if (!read(file, p, c))
                        return false;
                    p += 2u;
                    for (uint16_t i = 0; i < code; ++i)
                        putColor(rgba, c);
                }
                else
                    for (uint32_t i = 0; i < 0x10000u - code && p + 2u <= file.size(); ++i, p += 2u)
                    {
                        uint16_t c = 0;
                        read(file, p, c);
                        putColor(rgba, c);
                    }
            }
            rgba.resize(pixels * 4u, 0u);
            return true;
        }
    }

    std::optional<Icon> parse(const std::vector<uint8_t> &file)
    {
        uint32_t magic = 0, shapeCount = 0, vertexCount = 0;
        if (!read(file, 0u, magic) || magic != kMagic || !read(file, 4u, shapeCount) || !read(file, 16u, vertexCount) ||
            shapeCount == 0u || shapeCount > 16u)
            return std::nullopt;
        const size_t stride = 8u * shapeCount + 8u + 4u + 4u;
        size_t at = kHeader;
        if (at + stride * vertexCount > file.size())
            return std::nullopt;
        Icon icon;
        icon.shapes.assign(shapeCount, {});
        for (uint32_t v = 0; v < vertexCount; ++v, at += stride)
        {
            int16_t q[4];
            for (uint32_t s = 0; s < shapeCount; ++s)
            {
                std::memcpy(q, file.data() + at + 8u * s, 8u);
                icon.shapes[s].push_back({q[0] / 4096.0f, q[1] / 4096.0f, q[2] / 4096.0f});
            }
            const size_t n = at + 8u * shapeCount;
            std::memcpy(q, file.data() + n, 8u);
            icon.normals.push_back({q[0] / 4096.0f, q[1] / 4096.0f, q[2] / 4096.0f});
            std::memcpy(q, file.data() + n + 8u, 4u);
            icon.uvs.push_back({q[0] / 4096.0f, q[1] / 4096.0f});
            std::array<uint8_t, 4> c{};
            std::memcpy(c.data(), file.data() + n + 12u, 4u);
            icon.colors.push_back(c);
        }
        // The animation: an id, a frame length, a speed, an offset, a frame
        // count, then each frame's shape, key count, two words and keys.
        uint32_t frames = 0;
        if (!read(file, at + 4u, icon.frameLength) || !read(file, at + 8u, icon.speed) || !read(file, at + 16u, frames))
            return std::nullopt;
        // Sources disagree on whether a frame stores all its keys or one
        // less: the one whose picture then ends the file is right.
        for (const uint32_t less : {0u, 1u})
            if (const size_t past = pastAnimation(file, at + 20u, frames, less);
                past != 0u && picture(file, past, icon.rgba))
                return icon;
        return std::nullopt;
    }
}
