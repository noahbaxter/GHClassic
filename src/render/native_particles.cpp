// PsParticleSys, native: each live particle as a camera-facing square.
//
// Retail PsParticleSys::DrawShowing (0x1a2c28) draws while the system has live
// particles (+0x110) or a preserved packet (+0x2d0). It brings the system's
// world transform (+0x1f0) up to date (RndParticleSys::UpdateRelativeXfm,
// 0x1cf7b0), hands it to VU1 as qw676..679, and sends every particle in the
// list at +0x110 (next at +0x6c) through VU1 program 0x5cc once per pass of its
// material (+0x1e4, else the default one) as a GS sprite (DrawSprites
// 0x1a2a40): its colour (+0x00) and its position (+0x20) with half its size
// (+0x60) in w.
//
// Program 0x5cc projects the position through the world transform and the
// camera, then puts the sprite's corners half a size either way along the
// camera's right and up axes, in clip space, so the square keeps its world
// size at any distance. The first corner, left and above, takes uv (0, 0) and
// the other (1, 1): v runs down the screen, as a texture's rows do. The
// colour is the particle's times the material's colour scale (qw695), and
// times the environ's ambient when the material uses the environ (qw688 w):
// nothing else lights a particle.

#include "render/native_particles.h"

#include "guest.h"
#include "milo/layout.h"
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "render/camera.h"
#include "render/frame.h"
#include "render/native_environ.h"
#include "render/native_mat.h"
#include "render/native_mesh.h"

#include <memory>

namespace gh2
{
    namespace
    {
        const Addresses *s_addresses = nullptr;

        constexpr uint32_t kFirstParticle = 0x110u; // RndParticle*, the live list
        constexpr uint32_t kMat = 0x1e4u;           // RndMat* (ObjPtr at +0x1dc)
        constexpr uint32_t kWorld = 0x1f0u;         // Transform, as UpdateRelativeXfm leaves it
        constexpr uint32_t kPreserved = 0x2d0u;     // MemHandle*, the preserved sprite packet
        constexpr uint32_t kPreservedQwords = 0x2d4u; // its length
        constexpr uint32_t kHandleData = 0x10u;     // a MemHandle's first word, plus this, is its memory (Lock 0x2cede8)
        constexpr uint32_t kParticleColor = 0x00u;  // 4 floats
        constexpr uint32_t kParticlePos = 0x20u;    // 3 floats, in the system's space
        constexpr uint32_t kParticleSize = 0x60u;   // float
        constexpr uint32_t kParticleNext = 0x6cu;   // RndParticle*

        void drawShowing(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t returnTo = GPR_U32(ctx, 31);
            const uint32_t sys = GPR_U32(ctx, 4);
            const uint32_t first = load<uint32_t>(rdram, sys + kFirstParticle);
            // DrawSprites (0x1a2a64) sends a preserved packet in place of the
            // live list.
            const uint32_t preserved = load<uint32_t>(rdram, sys + kPreserved);
            if (first == 0u && preserved == 0u)
            {
                ctx->pc = returnTo;
                return;
            }
            runtime->callGuestFunction(rdram, ctx, s_addresses->updateRelativeXfm, {sys});

            const Matrix world = readTransform(rdram, sys + kWorld);
            const uint32_t cameraIndex = currentCamera(rdram);
            const Matrix &view = building().cameras[cameraIndex].view;
            // The camera's world axes: the columns of its inverse.
            const float right[3] = {view[0], view[4], view[8]};
            const float up[3] = {view[2], view[6], view[10]};

            uint32_t mat = load<uint32_t>(rdram, sys + kMat);
            if (mat == 0u)
                mat = load<uint32_t>(rdram, s_addresses->defaultMat);
            const bool lit = mat != 0u && load<uint32_t>(rdram, mat + milo::mat::kUseEnviron) != 0u;
            const float *ambient = currentEnviron().ambient;

            auto mesh = std::make_shared<MeshData>();
            const auto addSprite = [&](uint32_t colorAt, uint32_t posAt, float half) {
                if (mesh->verts.size() + 4u > 0x10000u)
                    return;
                float local[3];
                for (uint32_t c = 0; c < 3; ++c)
                    local[c] = load<float>(rdram, posAt + c * 4u);
                float center[3];
                for (uint32_t c = 0; c < 3; ++c)
                    center[c] = local[0] * world[0 * 4 + c] + local[1] * world[1 * 4 + c] +
                                local[2] * world[2 * 4 + c] + world[3 * 4 + c];
                float color[4];
                for (uint32_t c = 0; c < 4; ++c)
                    color[c] = load<float>(rdram, colorAt + c * 4u);
                if (lit)
                    for (uint32_t c = 0; c < 3; ++c)
                        color[c] *= ambient[c];

                // Left below, left above, right below, right above.
                const float sx[4] = {-1.0f, -1.0f, 1.0f, 1.0f};
                const float sy[4] = {-1.0f, 1.0f, -1.0f, 1.0f};
                const uint16_t base = static_cast<uint16_t>(mesh->verts.size());
                for (int i = 0; i < 4; ++i)
                {
                    Vertex v{};
                    for (uint32_t c = 0; c < 3; ++c)
                        v.pos[c] = center[c] + half * (sx[i] * right[c] + sy[i] * up[c]);
                    for (uint32_t c = 0; c < 4; ++c)
                        v.color[c] = color[c];
                    v.uv[0] = sx[i] < 0.0f ? 0.0f : 1.0f;
                    v.uv[1] = sy[i] < 0.0f ? 1.0f : 0.0f;
                    mesh->verts.push_back(v);
                }
                for (uint16_t i : {0, 1, 2, 2, 1, 3})
                    mesh->indices.push_back(static_cast<uint16_t>(base + i));
            };
            if (preserved != 0u)
            {
                // The packet as UpdateSpritePacket (0x1a2598) left it, in
                // batches of up to 127: two header qwords, the count in the
                // second's first word, a colour qword and a position qword
                // (half the size in w) for each sprite, then the qword that
                // runs the program.
                uint32_t at = load<uint32_t>(rdram, preserved) + kHandleData;
                const uint32_t end = at + load<uint32_t>(rdram, sys + kPreservedQwords) * 0x10u;
                while (at + 0x30u <= end)
                {
                    const uint32_t count = load<uint32_t>(rdram, at + 0x10u);
                    at += 0x20u;
                    if (count > 0x7fu || at + count * 0x20u + 0x10u > end)
                        break;
                    for (uint32_t i = 0; i < count; ++i, at += 0x20u)
                        addSprite(at, at + 0x10u, load<float>(rdram, at + 0x1cu));
                    at += 0x10u;
                }
            }
            else
            {
                for (uint32_t p = first; p != 0u; p = load<uint32_t>(rdram, p + kParticleNext))
                    addSprite(p + kParticleColor, p + kParticlePos, load<float>(rdram, p + kParticleSize) * 0.5f);
            }

            if (!mesh->indices.empty())
            {
                DrawCall draw;
                draw.mesh = mesh;
                draw.camera = cameraIndex;
                draw.world = identity();
                draw.lightWorld = identity();
                for (uint32_t pass = mat; pass != 0u; pass = nextPass(rdram, pass))
                {
                    draw.material = readMaterial(rdram, pass);
                    draw.material.highlight = false; // TEX0's TFX is put back to modulate (0x1a2da0)
                    // The colours are final; sprites take their uvs as given.
                    draw.material.useEnviron = false;
                    draw.material.prelit = true;
                    draw.material.texGen = milo::mat::kTexGenNone;
                    draw.material.uvXfm[0] = 1.0f;
                    draw.material.uvXfm[1] = 0.0f;
                    draw.material.uvXfm[2] = 0.0f;
                    draw.material.uvXfm[3] = 1.0f;
                    draw.material.uvXfm[4] = 0.0f;
                    draw.material.uvXfm[5] = 0.0f;
                    building().draws.push_back(draw);
                }
            }
            ctx->pc = returnTo;
        }
    }

    void installNativeParticles(PS2Runtime &runtime, const Addresses &addresses)
    {
        s_addresses = &addresses;
        runtime.replaceFunction(addresses.psParticleSysDrawShowing, drawShowing);
    }
}
