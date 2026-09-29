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
#include "render/texture_capture.h"

namespace gh2
{
    namespace
    {
        // The uv transform PsMat::Update builds from tex_xfm for the xfm tex
        // gens (0x19d510), rows at +0x170/+0x180 and offset at +0x1a0. The
        // off-diagonals flip sign, and xfm turns about the texture's centre
        // where xfm origin keeps tex_xfm's position as is.
        void readUvXfm(uint8_t *rdram, uint32_t mat, Material &m)
        {
            const uint32_t xfm = mat + milo::mat::kTexXfm;
            const float m00 = load<float>(rdram, xfm + 0x00u);
            const float m01 = load<float>(rdram, xfm + 0x04u);
            const float m10 = load<float>(rdram, xfm + 0x10u);
            const float m11 = load<float>(rdram, xfm + 0x14u);
            const float px = load<float>(rdram, xfm + 0x30u);
            const float py = load<float>(rdram, xfm + 0x34u);
            m.uvXfm[0] = m00;
            m.uvXfm[1] = -m01;
            m.uvXfm[2] = -m10;
            m.uvXfm[3] = m11;
            if (m.texGen == milo::mat::kTexGenXfm)
            {
                const float x = -px - 0.5f;
                const float y = py - 0.5f;
                m.uvXfm[4] = x * m00 + y * -m10 + 0.5f;
                m.uvXfm[5] = x * -m01 + y * m11 + 0.5f;
            }
            else
            {
                m.uvXfm[4] = px;
                m.uvXfm[5] = py;
            }
        }

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
        m.useEnviron = load<uint32_t>(rdram, mat + milo::mat::kUseEnviron) != 0u;
        m.prelit = load<uint32_t>(rdram, mat + milo::mat::kPrelit) != 0u;
        // Update drops the texture for a dest-blended material.
        if (m.blend != milo::mat::kBlendDest)
        {
            const uint32_t tex = load<uint32_t>(rdram, mat + milo::mat::kDiffuseTex);
            m.texture = capturedTexture(tex);
            if (tex != 0u && (load<uint32_t>(rdram, tex + milo::tex::kType) & milo::tex::kTypeRendered) != 0u)
                m.renderTarget = tex;
        }
        m.texGen = load<uint32_t>(rdram, mat + milo::mat::kTexGen);
        if (m.texGen == milo::mat::kTexGenXfm || m.texGen == milo::mat::kTexGenXfmOrigin)
            readUvXfm(rdram, mat, m);
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
