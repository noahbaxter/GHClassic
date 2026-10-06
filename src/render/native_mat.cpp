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
#include "ps2_runtime.h"
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

        // What PsMat::Update's environ case (0x19d60c) leaves at +0x170: tex_xfm's
        // rotation transposed, times rows (1 0 0) (0 0 1) (0 -1 0). Select hands
        // it to VU1 as qw691..693 for program 0x139.
        void readEnvRows(uint8_t *rdram, uint32_t mat, Material &m)
        {
            const uint32_t xfm = mat + milo::mat::kTexXfm;
            for (uint32_t i = 0; i < 3; ++i)
            {
                m.envRows[i][0] = load<float>(rdram, xfm + 0x00u + i * 4u);
                m.envRows[i][1] = -load<float>(rdram, xfm + 0x20u + i * 4u);
                m.envRows[i][2] = load<float>(rdram, xfm + 0x10u + i * 4u);
            }
        }

        // What PsMat::Update's projected case (0x19d374) leaves at +0x170 and
        // +0x1a0: tex_xfm's rotation through FastInvert (0x2daf60: transposed,
        // each of its rows over its length squared), tex_xfm's position
        // negated through that, and both times rows (1 0 0) (0 0 1) (0 -1 0).
        // Select hands them to VU1 as qw691..694 for program 0x410.
        void readProjRows(uint8_t *rdram, uint32_t mat, Material &m)
        {
            const uint32_t xfm = mat + milo::mat::kTexXfm;
            float inverse[3][3];
            for (uint32_t j = 0; j < 3; ++j)
            {
                float row[3];
                for (uint32_t i = 0; i < 3; ++i)
                    row[i] = load<float>(rdram, xfm + j * 0x10u + i * 4u);
                const float scale = 1.0f / (row[0] * row[0] + row[1] * row[1] + row[2] * row[2]);
                for (uint32_t i = 0; i < 3; ++i)
                    inverse[i][j] = row[i] * scale;
            }
            float offset[3] = {};
            for (uint32_t k = 0; k < 3; ++k)
            {
                const float p = -load<float>(rdram, xfm + 0x30u + k * 4u);
                for (uint32_t c = 0; c < 3; ++c)
                    offset[c] += p * inverse[k][c];
            }
            for (uint32_t i = 0; i < 4; ++i)
            {
                const float *row = i < 3 ? inverse[i] : offset;
                m.projRows[i][0] = row[0];
                m.projRows[i][1] = -row[2];
                m.projRows[i][2] = row[1];
            }
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
        m.alphaWrite = load<uint32_t>(rdram, mat + milo::mat::kAlphaWrite) != 0u;
        m.destAlphaTest = load<uint32_t>(rdram, mat + milo::mat::kDestAlphaTest) != 0u;
        m.texWrap = load<uint32_t>(rdram, mat + milo::mat::kTexWrap) != 0u;
        m.useEnviron = load<uint32_t>(rdram, mat + milo::mat::kUseEnviron) != 0u;
        m.prelit = load<uint32_t>(rdram, mat + milo::mat::kPrelit) != 0u;
        // Update drops the texture for a dest-blended material.
        if (m.blend != milo::mat::kBlendDest)
        {
            const uint32_t tex = load<uint32_t>(rdram, mat + milo::mat::kDiffuseTex);
            m.texture = capturedTexture(rdram, tex);
            const uint32_t type = tex != 0u ? load<uint32_t>(rdram, tex + milo::tex::kType) : 0u;
            if ((type & milo::tex::kTypeRendered) != 0u)
                m.renderTarget = tex;
            // Update: the add blend alone (0x19d064), textured and not prelit (0x19d2b0).
            m.highlight = tex != 0u && (type & milo::tex::kTypeFrameBuffer) == 0u &&
                          m.blend == milo::mat::kBlendAdd && !m.prelit;
        }
        m.texGen = load<uint32_t>(rdram, mat + milo::mat::kTexGen);
        if (m.texGen == milo::mat::kTexGenXfm || m.texGen == milo::mat::kTexGenXfmOrigin)
            readUvXfm(rdram, mat, m);
        else if (m.texGen == milo::mat::kTexGenEnviron)
            readEnvRows(rdram, mat, m);
        else if (m.texGen == milo::mat::kTexGenProjected)
            readProjRows(rdram, mat, m);
        if (m.blend >= milo::mat::kBlendCount)
            m.blend = milo::mat::kBlendSrcAlpha; // Update's default case
        if (m.zMode >= milo::mat::kZModeCount)
            m.zMode = milo::mat::kZNormal;
        return m;
    }

    // Select calls UpdateSphereXfm on every select of a sphere material
    // (0x3d8638) and hands VU1 what it leaves at 0x46d490 as qw691..694 for
    // program 0x347. It reads PsMat +0x170, where Update's sphere case
    // (0x19d5a8) keeps tex_xfm's rotation transposed; Update never runs
    // here, so that is written first.
    void readSphereRows(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, const Addresses &addresses,
                        uint32_t mat, Material &m)
    {
        const uint32_t xfm = mat + milo::mat::kTexXfm;
        for (uint32_t row = 0; row < 3; ++row)
            for (uint32_t c = 0; c < 3; ++c)
                store<float>(rdram, mat + milo::mat::kPsTexGenRows + row * 0x10u + c * 4u,
                             load<float>(rdram, xfm + c * 0x10u + row * 4u));
        runtime->callGuestFunction(rdram, ctx, addresses.psMatUpdateSphereXfm, {mat});
        for (uint32_t row = 0; row < 4; ++row)
            for (uint32_t c = 0; c < 3; ++c)
                m.sphereRows[row][c] = load<float>(rdram, addresses.sphereXfm + row * 0x10u + c * 4u);
        m.sphere = true;
    }

    uint32_t nextPass(uint8_t *rdram, uint32_t mat)
    {
        return load<uint32_t>(rdram, mat + milo::mat::kNextPass);
    }
}
