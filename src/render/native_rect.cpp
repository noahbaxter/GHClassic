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

#include <algorithm>
#include <iterator>
#include <memory>
#include <optional>

namespace gh2
{
    namespace
    {
        void drawRect(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            const uint32_t returnTo = GPR_U32(ctx, 31);
            const auto rnd = load<milo::Rnd>(rdram, GPR_U32(ctx, 4));
            const auto rect = load<milo::Rect>(rdram, GPR_U32(ctx, 5));
            const uint32_t color = GPR_U32(ctx, 6);
            const uint32_t mat = GPR_U32(ctx, 7);
            const uint32_t color2 = GPR_U32(ctx, 8);
            const uint32_t color3 = GPR_U32(ctx, 9);

            const float width = static_cast<float>(rnd.width);
            const float height = static_cast<float>(rnd.height);
            if (width <= 0.0f || height <= 0.0f)
            {
                ctx->pc = returnTo;
                return;
            }

            const auto material = mat != 0u ? std::optional(load<milo::RndMat>(rdram, mat)) : std::nullopt;
            milo::Color corner[4];
            if (material && material->prelit == 0u)
                std::fill(std::begin(corner), std::end(corner), material->color);
            else
            {
                std::fill(std::begin(corner), std::end(corner), load<milo::Color>(rdram, color));
                if (color2 != 0u)
                    corner[2] = corner[3] = load<milo::Color>(rdram, color2);
                else if (color3 != 0u)
                    corner[1] = corner[3] = load<milo::Color>(rdram, color3);
            }

            auto mesh = std::make_shared<MeshData>();
            const float px[4] = {rect.x, rect.x, rect.x + rect.w, rect.x + rect.w};
            const float py[4] = {rect.y, rect.y + rect.h, rect.y, rect.y + rect.h};
            const float uv[4][2] = {{0.0f, 0.0f}, {0.0f, 1.0f}, {1.0f, 0.0f}, {1.0f, 1.0f}};
            for (int i = 0; i < 4; ++i)
            {
                Vertex v{};
                v.pos[0] = px[i] / width * 2.0f - 1.0f;
                v.pos[1] = py[i] / height * 2.0f - 1.0f;
                v.pos[2] = 1.0f; // Z 0xffff: the nearest
                for (int c = 0; c < 4; ++c)
                    v.color[c] = corner[i].rgba[c];
                v.uv[0] = uv[i][0];
                v.uv[1] = uv[i][1];
                mesh->verts.push_back(v);
            }
            mesh->indices = {0, 1, 2, 2, 1, 3};

            DrawCall draw;
            draw.mesh = mesh;
            draw.screen = true;
            draw.camera = internCamera(Camera{}); // the whole screen: the verts are in clip space
            draw.world = identity();
            draw.lightWorld = identity();
            if (mat == 0u)
            {
                draw.material.blend = milo::RndMat::kBlendSrcAlpha;
                draw.material.zMode = milo::RndMat::kZDisable;
                draw.material.prelit = true;
                building().draws.push_back(draw);
            }
            for (uint32_t pass = mat; pass != 0u; pass = nextPass(rdram, pass))
            {
                draw.material = readMaterial(rdram, pass);
                // Never lit: the colours are final. Only tex_xfm moves the uvs.
                draw.material.useEnviron = false;
                draw.material.prelit = true;
                if (draw.material.texGen != milo::RndMat::kTexGenXfm && draw.material.texGen != milo::RndMat::kTexGenXfmOrigin)
                    draw.material.texGen = milo::RndMat::kTexGenNone;
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
