#include "hub/poster_meshes.h"

#include "gh1/scene.h"

#include <algorithm>
#include <cmath>

namespace gh2::poster_meshes
{
    namespace
    {
        // The guest's stand-in for a picture the host draws: its size only
        // counts in the guest's memory.
        constexpr uint32_t kStandIn = 64u;

        using gh1::putF32;

        // Where a body holds `name` as a string, its length word's offset.
        std::optional<size_t> stringAt(const milo::Bytes &body, const std::string &name)
        {
            for (size_t o = 0u; o + 4u + name.size() <= body.size(); ++o)
                if (milo::u32(body, o) == name.size() && std::memcmp(body.data() + o + 4u, name.data(), name.size()) == 0)
                    return o;
            return std::nullopt;
        }
    }

    std::optional<milo::Bytes> meshBody(const milo::Bytes &like, const std::string &likeMat, const Mesh &mesh,
                                        const std::string &name, const std::string &mat,
                                        const std::array<float, 3> *light, const std::array<float, 3> &tint)
    {
        const auto at = stringAt(like, likeMat);
        if (!at || *at < 20u)
            return std::nullopt;
        size_t o = *at;
        milo::str(like, o); // its Mat
        milo::str(like, o); // its geometry's owner
        const size_t words = o;
        milo::Bytes out(like.begin(), like.begin() + static_cast<std::ptrdiff_t>(*at));
        milo::putStr(out, mat);
        milo::putStr(out, name);
        out.insert(out.end(), like.begin() + static_cast<std::ptrdiff_t>(words),
                   like.begin() + static_cast<std::ptrdiff_t>(words + 8u));
        out.push_back(0u); // no BSP tree
        const uint32_t verts = static_cast<uint32_t>(mesh.verts.size() / 8u);
        milo::putU32(out, verts);
        for (uint32_t v = 0u; v < verts; ++v)
        {
            const float *f = mesh.verts.data() + v * 8u;
            for (int i = 0; i < 6; ++i)
                putF32(out, f[i]);
            const float shade =
                light ? 0.15f + 0.85f * std::fabs((*light)[0] * f[3] + (*light)[1] * f[4] + (*light)[2] * f[5]) : 1.0f;
            for (int i = 0; i < 3; ++i)
                putF32(out, shade * tint[i]);
            putF32(out, 1.0f);
            putF32(out, f[6]);
            putF32(out, f[7]);
        }
        const uint32_t faces = static_cast<uint32_t>(mesh.faces.size() / 3u);
        const auto *faceBytes = reinterpret_cast<const uint8_t *>(mesh.faces.data());
        milo::putU32(out, faces);
        out.insert(out.end(), faceBytes, faceBytes + 6u * faces);
        std::vector<uint32_t> groups;
        for (uint32_t left = faces; left > 0u; left -= std::min(left, 255u))
            groups.push_back(std::min(left, 255u));
        milo::putU32(out, static_cast<uint32_t>(groups.size()));
        for (const uint32_t g : groups)
            out.push_back(static_cast<uint8_t>(g));
        milo::putStr(out, ""); // no bones
        const milo::Bytes strips = milo::strips(faceBytes, groups);
        out.insert(out.end(), strips.begin(), strips.end());
        return out;
    }

    Mesh quad(float halfWide, float halfHigh, float u0, float u1)
    {
        Mesh out;
        out.verts = {-halfWide, 0, -halfHigh, 0, -1, 0, u0, 1, halfWide,  0, -halfHigh, 0, -1, 0, u1, 1,
                     halfWide,  0, halfHigh,  0, -1, 0, u1, 0, -halfWide, 0, halfHigh,  0, -1, 0, u0, 0};
        out.faces = {0, 1, 2, 0, 2, 3};
        return out;
    }

    milo::Bytes texOf(const milo::Bytes &like, const textures::Source &picture)
    {
        return gh1::texWith(like, textures::standIn(textures::add(picture), kStandIn, kStandIn), true);
    }
}
