#include "milo/objects.h"

#include <cstring>

namespace gh2::milo
{
    namespace
    {
        // Past the class revision and the Object block, to the class's own
        // fields. False for another revision, or an Object with type
        // properties, which are not read.
        bool enter(const Bytes &body, uint32_t version, uint32_t revision, size_t &o)
        {
            const bool xbox = version == 25u;
            if (body.size() < 8u || u32(body, 0u) != revision || u32(body, 4u) != (xbox ? 1u : 0u))
                return false;
            o = 8u;
            str(body, o); // its type
            if (o >= body.size() || body[o++] != 0u)
                return false;
            if (xbox)
                str(body, o); // its note
            return o < body.size();
        }
    }

    std::optional<Tex> readTex(const Bytes &body, uint32_t version)
    {
        size_t o = 0u;
        if (!enter(body, version, 10u, o))
            return std::nullopt;
        Tex tex;
        tex.bpp = o + 8u;
        o += 12u;
        tex.source = str(body, o);
        tex.type = u32(body, o + 4u);
        tex.bitmap = o + 9u;
        if (tex.bitmap > body.size())
            return std::nullopt;
        return tex;
    }

    std::optional<std::string> fontMat(const Bytes &body, uint32_t version)
    {
        size_t o = 0u;
        if (!enter(body, version, 15u, o))
            return std::nullopt;
        return str(body, o);
    }

    std::optional<Font> readFont(const Bytes &body, uint32_t version)
    {
        size_t o = 0u;
        if (!enter(body, version, 15u, o))
            return std::nullopt;
        str(body, o);
        if (o + 8u > body.size())
            return std::nullopt;
        Font font;
        font.cell = o;
        std::memcpy(&font.cellWidth, body.data() + o, 4u);
        std::memcpy(&font.cellHeight, body.data() + o + 4u, 4u);
        font.rest.assign(body.begin() + static_cast<std::ptrdiff_t>(o + 8u), body.end());
        const auto u32 = [&body](size_t at)
        {
            uint32_t v = 0;
            if (at + 4u <= body.size())
                std::memcpy(&v, body.data() + at, 4u);
            return v;
        };
        size_t at = o + 16u;
        const uint32_t n = u32(at);
        if (at + 4u + n >= body.size())
            return font;
        font.charsAt = at;
        font.chars.assign(reinterpret_cast<const char *>(body.data() + at + 4u), n);
        at += 4u + n;
        at = body[at] ? at + 5u + 8u * u32(at + 1u) : at + 1u;
        at += 4u + u32(at);
        if (at + 18u + 256u * 16u != body.size())
            return font;
        font.packed = body[at + 1u] != 0u;
        font.bitmapAt = at + 2u;
        font.infoAt = at + 18u;
        return font;
    }

    std::optional<std::string> matDiffuse(const Bytes &body, uint32_t version)
    {
        size_t o = 0u;
        if (!enter(body, version, version == 25u ? 28u : 27u, o))
            return std::nullopt;
        // Blend, colour, prelit, use environ, z mode, alpha cut, alpha
        // write, tex gen, tex wrap and tex xfm come first.
        o += 4u + 16u + 2u + 4u + 2u + 4u + 4u + 48u;
        if (o + 4u > body.size())
            return std::nullopt;
        return str(body, o);
    }
}
