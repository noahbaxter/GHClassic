// Frame boundaries for the native renderer. The guest's own PsRnd begin and
// end still run (pacing, bookkeeping, the null FlushPacket); these hooks only
// read what a frame needs on entry and hand it to the host.

#include "render/native_rnd.h"

#include "guest.h"
#include "hook.h"
#include "milo/layout.h"
#include "ps2_runtime_macros.h"
#include "render/camera.h"
#include "render/frame.h"

#include <memory>

namespace gh2
{
    namespace
    {
        constexpr float kYRatios[] = {1.0f, 0.75f, 0.5625f}; // Rnd::YRatio, by aspect index
        constexpr uint32_t kAspectWidescreen = 2u; // what Options::SetWideScreen(true) sets

        struct BeginTag;
        struct EndTag;

        void onBeginDrawing(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            const uint32_t rnd = GPR_U32(ctx, 4);
            Frame &frame = building();
            const uint64_t serial = frame.serial + 1u;
            previous() = std::move(frame);
            frame = Frame{};
            frame.serial = serial;
            for (uint32_t i = 0; i < 4; ++i)
                frame.clear[i] = load<float>(rdram, rnd + milo::rnd::kClearColor + i * 4u);
            const int32_t width = load<int32_t>(rdram, rnd + milo::rnd::kWidth);
            const int32_t height = load<int32_t>(rdram, rnd + milo::rnd::kHeight);
            if (width > 0 && height > 0)
            {
                frame.width = static_cast<uint32_t>(width);
                frame.height = static_cast<uint32_t>(height);
            }
            const uint32_t aspect = load<uint32_t>(rdram, rnd + milo::rnd::kAspect);
            if (aspect < 3u)
                frame.yRatio = kYRatios[aspect];
            frame.displayAspect = aspect == kAspectWidescreen ? 16.0f / 9.0f : 4.0f / 3.0f;
        }

        // Depth of field, which retail does to a finished frame from
        // PsRnd::VSync (0x19a9a0) when a camera shot has set a focus depth
        // (CamShotFrame::Interp 0x266b8c, through SetDepthOfField 0x19a798):
        // four PsRnd::CopyBuf (0x19a340) of the picture into the other
        // buffer, each a quarter of it, from 0 and 3 pixels along and down,
        // then that copied back from 0 to 1.5 pixels short of the size,
        // where the Z buffer is no nearer than the focus depth.
        //
        // Here the picture is copied once and drawn back over itself as the
        // four quarters, under the same Z test. Retail blurs a frame with the
        // depth set while the next was polled; this uses the frame's own.
        //
        // Both of retail's copies filter, and the GS filters a sprite about
        // half a texel before the whole texel coordinate it is given at each
        // pixel. So a pixel of a quarter is the mean of the texel there and
        // the one before, each way, and the copy back does that again: per
        // quarter a mean of four samples a pixel apart, centred a pixel
        // before where the copy back's uv falls.
        //
        // The quarters are sent with vertex alpha 23 under HIGHLIGHT
        // (0x19a538, 0x19a620), which adds 23 to each: what four adds of a
        // quarter lose to the 16-bit buffer's five bits, not a brightening.
        // None is added here.
        void addDepthOfField(uint8_t *rdram, uint32_t rnd, Frame &frame)
        {
            const uint32_t focus = load<uint32_t>(rdram, rnd + milo::rnd::kFocusZ);
            if (focus == 0u || load<uint32_t>(rdram, rnd + milo::rnd::kNoDepthOfField) != 0u)
                return;
            const float width = static_cast<float>(frame.width);
            const float height = static_cast<float>(frame.height);
            frame.copies.push_back({frame.draws.size(), kScreenCopyTex, 0, 0, frame.width, frame.height});

            auto mesh = std::make_shared<MeshData>();
            const float corner[4][2] = {{0.0f, 0.0f}, {0.0f, 1.0f}, {1.0f, 0.0f}, {1.0f, 1.0f}};
            for (const auto &c : corner)
            {
                Vertex v{};
                v.pos[0] = c[0] * 2.0f - 1.0f;
                v.pos[1] = c[1] * 2.0f - 1.0f;
                v.pos[2] = static_cast<float>(focus) / 65535.0f; // the GS's Z, as ProjectZ (0x19c3e8) gives it
                v.color[0] = v.color[1] = v.color[2] = 0.25f;
                v.color[3] = 1.0f;
                v.uv[0] = c[0];
                v.uv[1] = c[1];
                mesh->verts.push_back(v);
            }
            mesh->indices = {0, 1, 2, 2, 1, 3};

            DrawCall draw;
            draw.mesh = mesh;
            draw.screen = true;
            draw.camera = internCamera(Camera{});
            draw.world = identity();
            draw.lightWorld = identity();
            draw.material.zMode = milo::mat::kZTransparent;
            draw.material.prelit = true;
            draw.material.texWrap = false;
            draw.material.renderTarget = kScreenCopyTex;
            const float scale[2] = {(width - 1.5f) / width, (height - 1.5f) / height};
            draw.material.uvXfm[0] = scale[0];
            draw.material.uvXfm[3] = scale[1];
            draw.material.spread = true;
            draw.material.color[0] = 1.0f / width;
            draw.material.color[1] = 1.0f / height;
            const float offset[4][2] = {{0.0f, 0.0f}, {0.0f, 3.0f}, {3.0f, 0.0f}, {3.0f, 3.0f}};
            for (int tap = 0; tap < 4; ++tap)
            {
                draw.material.blend = tap == 0 ? milo::mat::kBlendSrc : milo::mat::kBlendAdd;
                // The copy back reads pixel x at x * scale - 0.5 of the
                // quarters, whose own pixels are the picture's a half before
                // their offset; a uv here is taken at x + 0.5.
                draw.material.uvXfm[4] = (offset[tap][0] - 0.5f - 0.5f * scale[0]) / width;
                draw.material.uvXfm[5] = (offset[tap][1] - 0.5f - 0.5f * scale[1]) / height;
                frame.draws.push_back(draw);
            }
        }

        // Every draw of the frame has been recorded by the time EndDrawing
        // starts.
        void onEndDrawing(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            addDepthOfField(rdram, GPR_U32(ctx, 4), building());
            frames().publish(building());
        }
    }

    void installNativeRnd(PS2Runtime &runtime, const Addresses &addresses)
    {
        EntryHook<BeginTag>::install(runtime, addresses.psRndBeginDrawing, onBeginDrawing);
        EntryHook<EndTag>::install(runtime, addresses.psRndEndDrawing, onEndDrawing);
    }
}
