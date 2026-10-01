// PsMat, native: materials are read at draw time instead of turned into GS
// state.
//
// Retail PsMat::Select (0x3d8348) turns a material into GS registers and a
// VU1 block in the current packet. Every draw that selects a material
// (PsMesh, PsMultiMesh, PsParticleSys, PsRnd::DrawRect) is native, so Select
// is never reached; the draws read the RndMat through readMaterial instead.
//
// The colour VU1 hands the GS is vertex colour x material colour x the scale
// PsMat::Update sets at +0x160: 255 for rgb without a texture or with
// intensify, else 128, and 128 for alpha. The GS modulates a texture by it
// with 128 as 1.0, so here 1.0 is 128 throughout, and intensify doubles a
// textured colour.

#include "render/native_mat.h"

#include "guest.h"
#include "milo/layout.h"
#include "render/texture_capture.h"

#include <algorithm>
#include <iterator>

namespace gh2
{
    namespace
    {
        // The uv transform PsMat::Update builds from tex_xfm for the xfm tex
        // gens (0x19d510), rows at +0x170/+0x180 and offset at +0x1a0. The
        // off-diagonals flip sign, and xfm turns about the texture's centre
        // where xfm origin keeps tex_xfm's position as is.
        void readUvXfm(const milo::Transform &xfm, Material &m)
        {
            const float m00 = xfm.rows[0][0];
            const float m01 = xfm.rows[0][1];
            const float m10 = xfm.rows[1][0];
            const float m11 = xfm.rows[1][1];
            const float px = xfm.pos[0];
            const float py = xfm.pos[1];
            m.uvXfm[0] = m00;
            m.uvXfm[1] = -m01;
            m.uvXfm[2] = -m10;
            m.uvXfm[3] = m11;
            if (m.texGen == milo::RndMat::kTexGenXfm)
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

        // What PsMat::Update's environ case (0x19d60c) leaves at +0x170: tex_xfm's
        // rotation transposed, times rows (1 0 0) (0 0 1) (0 -1 0). Select hands
        // it to VU1 as qw691..693 for program 0x139.
        void readEnvRows(const milo::Transform &xfm, Material &m)
        {
            for (uint32_t i = 0; i < 3; ++i)
            {
                m.envRows[i][0] = xfm.rows[0][i];
                m.envRows[i][1] = -xfm.rows[2][i];
                m.envRows[i][2] = xfm.rows[1][i];
            }
        }
    }

    Material readMaterial(uint8_t *rdram, uint32_t address)
    {
        const auto mat = load<milo::RndMat>(rdram, address);
        Material m;
        m.blend = mat.blend;
        m.zMode = mat.zMode;
        std::copy(std::begin(mat.color.rgba), std::end(mat.color.rgba), m.color);
        m.intensify = mat.intensify != 0u;
        m.alphaCut = mat.alphaCut != 0u;
        m.texWrap = mat.texWrap != 0u;
        m.useEnviron = mat.useEnviron != 0u;
        m.prelit = mat.prelit != 0u;
        // Update drops the texture for a dest-blended material.
        if (m.blend != milo::RndMat::kBlendDest)
        {
            const uint32_t tex = mat.diffuseTex.ptr.address;
            m.texture = capturedTexture(tex);
            if (tex != 0u && (load<uint32_t>(rdram, tex + milo::tex::kType) & milo::tex::kTypeRendered) != 0u)
                m.renderTarget = tex;
        }
        m.texGen = mat.texGen;
        if (m.texGen == milo::RndMat::kTexGenXfm || m.texGen == milo::RndMat::kTexGenXfmOrigin)
            readUvXfm(mat.texXfm, m);
        else if (m.texGen == milo::RndMat::kTexGenEnviron)
            readEnvRows(mat.texXfm, m);
        if (m.blend >= milo::RndMat::kBlendCount)
            m.blend = milo::RndMat::kBlendSrcAlpha; // Update's default case
        if (m.zMode >= milo::RndMat::kZModeCount)
            m.zMode = milo::RndMat::kZNormal;
        return m;
    }

    uint32_t nextPass(uint8_t *rdram, uint32_t mat)
    {
        return load<milo::RndMat>(rdram, mat).nextPass.ptr.address;
    }
}
