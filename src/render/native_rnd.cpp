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
#include "render/look.h"
#include "render/texture_store.h"
#include "settings/settings.h"

#include <cmath>
#include <cstring>
#include <memory>
#include <string>

namespace gh2
{
    namespace
    {
        constexpr float kYRatios[] = {1.0f, 0.75f, 0.5625f}; // Rnd::YRatio, by aspect index
        constexpr uint32_t kAspectWidescreen = 2u; // what Options::SetWideScreen(true) sets

        struct BeginTag;
        struct EndTag;
        struct ShotTag;

        const Addresses *s_addresses = nullptr;

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
            if (focus == 0u || load<uint32_t>(rdram, rnd + milo::rnd::kNoDepthOfField) != 0u ||
                !settings::get(settings::kDepthOfField))
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

        // The 360's depth of field, which a converted shot's keyframes carry
        // (gh2x/classes.h): what the shot now running focuses on, from the
        // last CamShotFrame::Interp, kept as the PS2 keeps its own until the
        // next. The keyframe before and the one after, `t` of the way
        // between them.
        struct Focus
        {
            bool on = false;
            uint32_t camera = 0; // the RndCam's guest address
            float from[3] = {};
            float to[3] = {};
            float t = 0.0f;
            float blurDepth = 0.0f;
            float focusBlur = 0.0f;
            float maxBlur = 0.0f;
        };
        Focus s_focus;

        // Where a keyframe of a converted shot focuses: the target of the
        // same keyframe in the shot's focus shot, resolved by the game.
        // False when it has none. Calls into the guest, with its stack moved
        // down past a name for ObjectDir::FindObject.
        bool focalPoint(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t shot, uint32_t keyframe,
                        float out[3])
        {
            const uint32_t first = load<uint32_t>(rdram, shot + milo::camShot::kKeyframes);
            const uint32_t last = load<uint32_t>(rdram, shot + milo::camShot::kKeyframes + 4u);
            if (keyframe < first || keyframe >= last)
                return false;
            const uint32_t object = load<uint32_t>(rdram, shot + milo::camShot::kObject);
            const uint32_t name = load<uint32_t>(rdram, object + milo::object::kName);
            const uint32_t dir = load<uint32_t>(rdram, object + milo::object::kDir);
            if (name == 0u || dir == 0u)
                return false;
            const std::string focus = look::focusShot(reinterpret_cast<const char *>(getMemPtr(rdram, name)));
            const uint32_t sp = GPR_U32(ctx, 29);
            const uint32_t text = (sp - 0x40u - static_cast<uint32_t>(focus.size())) & ~0xfu;
            std::memcpy(getMemPtr(rdram, text), focus.c_str(), focus.size() + 1u);
            SET_GPR_U32(ctx, 29, text - 0x20u);
            bool found = false;
            // The focus shot is a CamShot too, so its Object is as far in.
            if (const uint32_t other = static_cast<uint32_t>(
                    runtime->callGuestFunction(rdram, ctx, s_addresses->objectDirFindObject, {dir, text, 0u}));
                other != 0u)
            {
                const uint32_t focusShot = other - (object - shot);
                const uint32_t at =
                    load<uint32_t>(rdram, focusShot + milo::camShot::kKeyframes) + (keyframe - first);
                if (at < load<uint32_t>(rdram, focusShot + milo::camShot::kKeyframes + 4u) &&
                    load<uint32_t>(rdram, at + milo::camShotFrame::kTargets) !=
                        load<uint32_t>(rdram, at + milo::camShotFrame::kTargets + 4u))
                {
                    runtime->callGuestFunction(rdram, ctx, s_addresses->camShotFrameUpdateTarget, {at});
                    for (uint32_t i = 0; i < 3u; ++i)
                        out[i] = load<float>(rdram, at + milo::camShotFrame::kTargetPosition + i * 4u);
                    found = true;
                }
            }
            SET_GPR_U32(ctx, 29, sp);
            return found;
        }

        // CamShotFrame::Interp(shot, next, t, blend, cam), as a shot sets
        // its camera each frame.
        void onShotInterp(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const EntryArgs args(ctx);
            const uint32_t from = args.a[0], shot = args.a[1], to = args.a[2];
            s_focus = Focus{};
            const auto blur = [&](uint32_t keyframe, uint32_t field) { return load<float>(rdram, keyframe + field); };
            if (blur(from, milo::camShotFrame::kMaxFocalBlur) < look::kShotBlurMark ||
                blur(to, milo::camShotFrame::kMaxFocalBlur) < look::kShotBlurMark)
                return;
            // One keyframe's target stands for both where the other has none.
            const bool hasFrom = focalPoint(rdram, ctx, runtime, shot, from, s_focus.from);
            const bool hasTo = focalPoint(rdram, ctx, runtime, shot, to, s_focus.to);
            args.restore(ctx);
            if (!hasFrom && !hasTo)
                return;
            if (!hasFrom)
                std::memcpy(s_focus.from, s_focus.to, sizeof(s_focus.from));
            if (!hasTo)
                std::memcpy(s_focus.to, s_focus.from, sizeof(s_focus.to));
            const float t = args.f12;
            const auto between = [&](uint32_t field) { return blur(from, field) + (blur(to, field) - blur(from, field)) * t; };
            s_focus.on = true;
            s_focus.camera = args.a[3];
            s_focus.t = t;
            s_focus.blurDepth = between(milo::camShotFrame::kNearBlur);
            s_focus.focusBlur = between(milo::camShotFrame::kFarBlur);
            s_focus.maxBlur = between(milo::camShotFrame::kMaxFocalBlur) - look::kShotBlurMark;
        }

        // gDepthOfField as the 360 sets it for a shot. From one Xenia frame
        // of battle (12.65369, -11.66636, 0, 0.5 with the blur depth and max
        // blur both 0.5, near 10, far 10000:
        // .planning/gh2x-look/DESIGN.md): the blur is 0 at the focal
        // distance and 1 at the blur depth's share of it, straight between
        // in the depth the projection gives, f/(f-n) - nf/(f-n)/w. The
        // frame pins both numbers at a blur depth of 0.5 only; that it
        // scales the distance, and that the focus blur multiplier adds its
        // share of the distance as RB3's does, are read off the names.
        void addFocus(Frame &frame)
        {
            if (!s_focus.on || s_focus.maxBlur <= 0.0f || s_focus.blurDepth <= 0.0f || s_focus.blurDepth >= 1.0f)
                return;
            const Camera *camera = nullptr;
            for (const Camera &c : frame.cameras)
                if (c.id == s_focus.camera && c.target == 0u && c.yFov != 0.0f)
                    camera = &c;
            if (!camera || camera->nearPlane <= 0.0f || camera->farPlane <= camera->nearPlane)
                return;
            const auto distance = [&](const float *point)
            {
                double sum = 0.0;
                for (int i = 0; i < 3; ++i)
                    sum += (static_cast<double>(point[i]) - camera->eye[i]) * (static_cast<double>(point[i]) - camera->eye[i]);
                return std::sqrt(sum);
            };
            const double from = distance(s_focus.from);
            const double focal = (from + (distance(s_focus.to) - from) * s_focus.t) * (1.0 + s_focus.focusBlur);
            if (focal <= 0.0)
                return;
            const double n = camera->nearPlane, f = camera->farPlane;
            const double range = f / (f - n), slope = n * f / (f - n);
            const double sharp = range - slope / focal;
            const double blurred = range - slope / (focal * s_focus.blurDepth);
            const double scale = 1.0 / (sharp - blurred);
            frame.depthOfField[0] = static_cast<float>(scale);
            frame.depthOfField[1] = static_cast<float>(-sharp * scale);
            frame.depthOfField[2] = 0.0f;
            frame.depthOfField[3] = s_focus.maxBlur;
            frame.focusCamera = camera->id;        }

        // Every draw of the frame has been recorded by the time EndDrawing
        // starts.
        void onEndDrawing(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            addDepthOfField(rdram, GPR_U32(ctx, 4), building());
            addFocus(building());
            building().luminance = look::luminance();
            frames().publish(building());
            textures::endFrame();
            look::endFrame();
        }
    }

    void installNativeRnd(PS2Runtime &runtime, const Addresses &addresses)
    {
        EntryHook<BeginTag>::install(runtime, addresses.psRndBeginDrawing, onBeginDrawing);
        EntryHook<EndTag>::install(runtime, addresses.psRndEndDrawing, onEndDrawing);
        s_addresses = &addresses;
        EntryHook<ShotTag>::install(runtime, addresses.camShotFrameInterp, onShotInterp);
    }
}
