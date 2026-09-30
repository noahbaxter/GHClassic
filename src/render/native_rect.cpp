// PsRnd::DrawRect, native: a screen-space rectangle, as flares, fades and
// overlays draw.
//
// Retail DrawRect(rect, color, mat, color2, color3) (0x19b050) takes the rect
// in the Rnd's pixels, x from the left and y from the top, and sends one
// four-vertex strip per material pass at Z 0xffff, the nearest the 16-bit Z
// buffer holds:
//
//     (x0, y0) uv (0, 0)   (x0, y1) uv (0, 1)   (x1, y0) uv (1, 0)   (x1, y1) uv (1, 1)
//
// A material that is not prelit colours all four corners with its own colour.
// Otherwise every corner takes `color`, the right two `color2` when given (a
// left to right ramp), else the bottom two `color3`. The colours go through
// MakeRGBAQ (0x1990a8), which scales them as the mesh path does, so they are
// the draw's vertex colours as they are. The uvs go through the material's
// tex_xfm when its tex gen uses one (program 0x17c); any other tex gen is
// ignored. With no material the rect alpha blends, Z test off.

#include "render/native_rect.h"

#include "guest.h"
#include "milo/layout.h"
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "render/camera.h"
#include "render/frame.h"
#include "render/native_mat.h"

#include <cstring>
#include <memory>

namespace gh2
{
    namespace
    {
        constexpr uint32_t kRndWidth = 0x40u;  // Rnd: the frame's width in pixels
        constexpr uint32_t kRndHeight = 0x44u; // and height

        void readColor(uint8_t *rdram, uint32_t address, float out[4])
        {
            for (uint32_t i = 0; i < 4; ++i)
                out[i] = load<float>(rdram, address + i * 4u);
        }

        // A camera that covers the whole screen, for draws already in clip space.
        uint32_t screenCamera()
        {
            Frame &frame = building();
            Camera camera;
            if (!frame.cameras.empty() && std::memcmp(&frame.cameras.back(), &camera, sizeof(Camera)) == 0)
                return static_cast<uint32_t>(frame.cameras.size() - 1u);
            frame.cameras.push_back(camera);
            return static_cast<uint32_t>(frame.cameras.size() - 1u);
        }

        void drawRect(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            const uint32_t returnTo = GPR_U32(ctx, 31);
            const uint32_t rnd = GPR_U32(ctx, 4);
            const uint32_t rect = GPR_U32(ctx, 5);
            const uint32_t color = GPR_U32(ctx, 6);
            const uint32_t mat = GPR_U32(ctx, 7);
            const uint32_t color2 = GPR_U32(ctx, 8);
            const uint32_t color3 = GPR_U32(ctx, 9);

            const float width = static_cast<float>(load<int32_t>(rdram, rnd + kRndWidth));
            const float height = static_cast<float>(load<int32_t>(rdram, rnd + kRndHeight));
            const float x = load<float>(rdram, rect + 0x0u);
            const float y = load<float>(rdram, rect + 0x4u);
            const float w = load<float>(rdram, rect + 0x8u);
            const float h = load<float>(rdram, rect + 0xcu);
            if (width <= 0.0f || height <= 0.0f)
            {
                ctx->pc = returnTo;
                return;
            }

            float corner[4][4];
            if (mat != 0u && load<uint32_t>(rdram, mat + milo::mat::kPrelit) == 0u)
            {
                for (auto &c : corner)
                    readColor(rdram, mat + milo::mat::kColor, c);
            }
            else
            {
                for (auto &c : corner)
                    readColor(rdram, color, c);
                if (color2 != 0u)
                {
                    readColor(rdram, color2, corner[2]);
                    readColor(rdram, color2, corner[3]);
                }
                else if (color3 != 0u)
                {
                    readColor(rdram, color3, corner[1]);
                    readColor(rdram, color3, corner[3]);
                }
            }

            auto mesh = std::make_shared<MeshData>();
            const float px[4] = {x, x, x + w, x + w};
            const float py[4] = {y, y + h, y, y + h};
            const float uv[4][2] = {{0.0f, 0.0f}, {0.0f, 1.0f}, {1.0f, 0.0f}, {1.0f, 1.0f}};
            for (int i = 0; i < 4; ++i)
            {
                Vertex v{};
                v.pos[0] = px[i] / width * 2.0f - 1.0f;
                v.pos[1] = py[i] / height * 2.0f - 1.0f;
                v.pos[2] = 1.0f; // Z 0xffff: the nearest
                for (int c = 0; c < 4; ++c)
                    v.color[c] = corner[i][c];
                v.uv[0] = uv[i][0];
                v.uv[1] = uv[i][1];
                mesh->verts.push_back(v);
            }
            mesh->indices = {0, 1, 2, 2, 1, 3};

            DrawCall draw;
            draw.mesh = mesh;
            draw.screen = true;
            draw.camera = screenCamera();
            draw.world = identity();
            draw.lightWorld = identity();
            if (mat == 0u)
            {
                draw.material.blend = milo::mat::kBlendSrcAlpha;
                draw.material.zMode = milo::mat::kZDisable;
                draw.material.prelit = true;
                building().draws.push_back(draw);
            }
            for (uint32_t pass = mat; pass != 0u; pass = nextPass(rdram, pass))
            {
                draw.material = readMaterial(rdram, pass);
                // Never lit: the colours are final. Only tex_xfm moves the uvs.
                draw.material.useEnviron = false;
                draw.material.prelit = true;
                if (draw.material.texGen != milo::mat::kTexGenXfm && draw.material.texGen != milo::mat::kTexGenXfmOrigin)
                    draw.material.texGen = milo::mat::kTexGenNone;
                building().draws.push_back(draw);
            }
            ctx->pc = returnTo;
        }
    }

    void installNativeRect(PS2Runtime &runtime, const Addresses &addresses)
    {
        runtime.replaceFunction(addresses.psRndDrawRect, drawRect);
    }
}
