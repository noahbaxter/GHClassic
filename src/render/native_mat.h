#pragma once

#include "addresses.h"
#include "render/scene.h"

#include <cstdint>

class PS2Runtime;
struct R5900Context;

namespace gh2
{
    // The material a pass draws with, read from a RndMat. Game thread only.
    Material readMaterial(uint8_t *rdram, uint32_t mat);

    // A sphere tex gen material's rows for the camera being drawn through,
    // by the engine's own PsMat::UpdateSphereXfm.
    void readSphereRows(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, const Addresses &addresses,
                        uint32_t mat, Material &m);

    // The pass after `mat`, or 0.
    uint32_t nextPass(uint8_t *rdram, uint32_t mat);

    // Keeps what RndMat::Load drops of a GH1 material, for readMaterial.
    void installNativeMat(PS2Runtime &runtime, const Addresses &addresses);
}
