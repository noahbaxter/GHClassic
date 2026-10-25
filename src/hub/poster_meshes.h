#pragma once

// Meshes and pictures added to one of GH2's scenes by copying its own: the
// campaign picker's cases on the amp poster (content/campaign_case.h) and
// the quickplay pre-show's venues among character select's portraits
// (content/preshow.h).

#include "milo/milo.h"
#include "render/texture_store.h"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace gh2::poster_meshes
{
    struct Mesh
    {
        std::vector<float> verts; // 8 a vert: position, normal, uv
        std::vector<uint16_t> faces;
    };

    // A Mesh body made from `like`, a Mesh 28 of the scene drawn with the Mat
    // `likeMat`, as RndMesh::Load (0x3d5420) reads it: `like`'s own up to its
    // Mat's name, which keeps its Object, Trans and Draw, then `mat`, `name`
    // as the geometry's owner, `like`'s mutable and volume words and no BSP,
    // the verts (position, normal, colour, uv), the faces, their groups of up
    // to 255 and no bones, then their PS2 strips (milo::strips). No light
    // reaches a menu, so the colour is the light baked in: `light` (toward
    // it, of length 1, from either side) on each vert's normal, or flat
    // without one, times `tint`.
    std::optional<milo::Bytes> meshBody(const milo::Bytes &like, const std::string &likeMat, const Mesh &mesh,
                                        const std::string &name, const std::string &mat,
                                        const std::array<float, 3> *light = nullptr,
                                        const std::array<float, 3> &tint = {1.0f, 1.0f, 1.0f});

    // A flat picture 2 half-wide by 2 half-high, facing -y, z up, its uvs
    // across from u0 to u1.
    Mesh quad(float halfWide, float halfHigh, float u0 = 0.0f, float u1 = 1.0f);

    // A Tex like `like` whose bitmap is a small stand-in for that picture,
    // which the host draws at its own size (render/texture_store.h).
    milo::Bytes texOf(const milo::Bytes &like, const textures::Source &picture);
}
