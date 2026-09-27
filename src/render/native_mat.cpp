// PsMat, native: materials are read at draw time instead of turned into GS
// state.
//
// Retail PsMat::Select (0x3d8348) rebuilds the GS registers from the RndMat
// through PsMat::Update when the material is dirty, writes them and a VU1
// block (the colour at +0x30, then a colour scale) into the current packet,
// ORs PRIM bits into the caller's tag and returns the next pass (+0xb0). The
// packet is never sent, so the native one only returns the next pass. Its
// other callers (PsMultiMesh, PsParticleSys and PsRnd::DrawRect) still build
// packets around it, which nothing reads.
//
// The colour VU1 hands the GS is vertex colour x material colour x the scale
// PsMat::Update sets at +0x160: 255 for rgb without a texture or with
// intensify, else 128, and 128 for alpha. The GS modulates a texture by it
// with 128 as 1.0, so here 1.0 is 128 throughout, and intensify doubles a
// textured colour.

#include "render/native_mat.h"

#include "guest.h"
#include "milo/layout.h"
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"

namespace gh2
{
    namespace
    {
        void select(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            SET_GPR_U32(ctx, 2, nextPass(rdram, GPR_U32(ctx, 4)));
            ctx->pc = GPR_U32(ctx, 31);
        }
    }

    Material readMaterial(uint8_t *rdram, uint32_t mat)
    {
        Material m;
        m.blend = load<uint32_t>(rdram, mat + milo::mat::kBlend);
        m.zMode = load<uint32_t>(rdram, mat + milo::mat::kZMode);
        for (uint32_t i = 0; i < 4; ++i)
            m.color[i] = load<float>(rdram, mat + milo::mat::kColor + i * 4u);
        m.intensify = load<uint32_t>(rdram, mat + milo::mat::kIntensify) != 0u;
        m.alphaCut = load<uint32_t>(rdram, mat + milo::mat::kAlphaCut) != 0u;
        m.texWrap = load<uint32_t>(rdram, mat + milo::mat::kTexWrap) != 0u;
        // Update drops the texture for a dest-blended material.
        if (m.blend != milo::mat::kBlendDest)
            m.texture = load<uint32_t>(rdram, mat + milo::mat::kDiffuseTex);
        if (m.blend >= milo::mat::kBlendCount)
            m.blend = milo::mat::kBlendSrcAlpha; // Update's default case
        if (m.zMode >= milo::mat::kZModeCount)
            m.zMode = milo::mat::kZNormal;
        return m;
    }

    uint32_t nextPass(uint8_t *rdram, uint32_t mat)
    {
        return load<uint32_t>(rdram, mat + milo::mat::kNextPass);
    }

    void installNativeMat(PS2Runtime &runtime, const Addresses &addresses)
    {
        runtime.replaceFunction(addresses.psMatSelect, select);
    }
}
