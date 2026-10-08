#include "milo/objects.h"

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
