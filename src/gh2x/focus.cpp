#include "gh2x/focus.h"

#include "guest.h"
#include "hook.h"
#include "milo/layout.h"
#include "ps2_runtime_macros.h"
#include "render/frame.h"

#include <cmath>
#include <cstring>

namespace gh2::gh2x
{
    namespace
    {
        struct ShotTag;
        struct EndTag;

        const Addresses *s_addresses = nullptr;

        // What the shot now running focuses on, from the last
        // CamShotFrame::Interp, kept as the PS2 keeps its own depth of field
        // until the next. The keyframe before and the one after, `t` of the
        // way between them.
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
            const uint32_t first = load<uint32_t>(rdram, shot + ::milo::camShot::kKeyframes);
            const uint32_t last = load<uint32_t>(rdram, shot + ::milo::camShot::kKeyframes + 4u);
            if (keyframe < first || keyframe >= last)
                return false;
            const uint32_t object = load<uint32_t>(rdram, shot + ::milo::camShot::kObject);
            const uint32_t name = load<uint32_t>(rdram, object + ::milo::object::kName);
            const uint32_t dir = load<uint32_t>(rdram, object + ::milo::object::kDir);
            if (name == 0u || dir == 0u)
                return false;
            const std::string focus = focusShot(reinterpret_cast<const char *>(getMemPtr(rdram, name)));
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
                const uint32_t twin = other - (object - shot);
                const uint32_t at = load<uint32_t>(rdram, twin + ::milo::camShot::kKeyframes) + (keyframe - first);
                if (at < load<uint32_t>(rdram, twin + ::milo::camShot::kKeyframes + 4u) &&
                    load<uint32_t>(rdram, at + ::milo::camShotFrame::kTargets) !=
                        load<uint32_t>(rdram, at + ::milo::camShotFrame::kTargets + 4u))
                {
                    runtime->callGuestFunction(rdram, ctx, s_addresses->camShotFrameUpdateTarget, {at});
                    for (uint32_t i = 0; i < 3u; ++i)
                        out[i] = load<float>(rdram, at + ::milo::camShotFrame::kTargetPosition + i * 4u);
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
            if (blur(from, ::milo::camShotFrame::kMaxFocalBlur) < kShotBlurMark ||
                blur(to, ::milo::camShotFrame::kMaxFocalBlur) < kShotBlurMark)
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
            const auto between = [&](uint32_t field)
            { return blur(from, field) + (blur(to, field) - blur(from, field)) * t; };
            s_focus.on = true;
            s_focus.camera = args.a[3];
            s_focus.t = t;
            s_focus.blurDepth = between(::milo::camShotFrame::kNearBlur);
            s_focus.focusBlur = between(::milo::camShotFrame::kFarBlur);
            s_focus.maxBlur = between(::milo::camShotFrame::kMaxFocalBlur) - kShotBlurMark;
        }

        // gDepthOfField as the 360 sets it for a shot, into the frame about
        // to be published. From one Xenia frame of battle (12.65369,
        // -11.66636, 0, 0.5 with the blur depth and max blur both 0.5, near
        // 10, far 10000: .planning/gh2x-look/DESIGN.md): the blur is 0 at
        // the focal distance and 1 at the blur depth's share of it, straight
        // between in the depth the projection gives, f/(f-n) - nf/(f-n)/w.
        // The frame pins both numbers at a blur depth of 0.5 only; that it
        // scales the distance, and that the focus blur multiplier adds its
        // share of the distance as RB3's does, are read off the names.
        void onEndDrawing(uint8_t *, R5900Context *, PS2Runtime *)
        {
            Frame &frame = building();
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
                    sum += (static_cast<double>(point[i]) - camera->eye[i]) *
                           (static_cast<double>(point[i]) - camera->eye[i]);
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
            frame.focusCamera = camera->id;
        }
    }

    void installFocus(PS2Runtime &runtime, const Addresses &addresses)
    {
        s_addresses = &addresses;
        EntryHook<ShotTag>::install(runtime, addresses.camShotFrameInterp, onShotInterp);
        EntryHook<EndTag>::install(runtime, addresses.psRndEndDrawing, onEndDrawing);
    }
}
