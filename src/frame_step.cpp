// GH2 times nearly everything by TaskMgr's clocks, which count guest cycles,
// so a shorter vblank only makes more frames of the same game. Three things
// step once per call instead, tuned for one call a PS2 frame:
//
// - CharHair::Poll (0x176fb8) integrates with a fixed dt of 1/60.
// - CamShot::Shake (0x262f38) moves a spring by fixed gains and rolls a
//   random kick each call.
// - RndFlare::DrawFlare (0x1f9330) fades by one of +0x124 steps each draw.
//
// Each frame here covers some number of 60 Hz steps, counted in vblanks at
// EndDrawing, so the three run once per step: not at all in some frames above
// 60, more than once in a slow one. At 60 that is one step a frame, as
// retail.
//
// Poll keeps the hair's state in its points and sets only the bones' world
// transforms, so they fall back to rest once the head moves. Above 60 each
// frame poses them between the last two steps, moved to each strand's root.

#include "frame_step.h"

#include "guest.h"
#include "hook.h"
#include "milo/layout.h"
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "render/native_mesh.h"
#include "runtime/ee_scheduler.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <unordered_map>
#include <vector>

namespace gh2
{
    namespace
    {
        // The runtime's default vblank, so that rate gives exactly one step
        // a vblank.
        constexpr int64_t kStepNs = 16667000;
        // A hitch (a load, a stall) is not worth replaying.
        constexpr int kMaxSteps = 4;

        const Addresses *s_addresses = nullptr;
        uint64_t s_lastVSync = 0;
        int64_t s_carryNs = 0;
        // For the frame being built.
        int s_steps = 1;
        bool s_between = false; // vblanks shorter than a step
        float s_since = 0.0f;   // the time since the last step, in steps

        struct EndDrawingTag;
        void onEndDrawing(uint8_t *, R5900Context *, PS2Runtime *runtime)
        {
            const EeScheduler &scheduler = runtime->eeScheduler();
            const uint64_t vsync = scheduler.currentVSyncTick();
            s_carryNs += static_cast<int64_t>(vsync - s_lastVSync) * scheduler.vblankPeriod().count();
            s_lastVSync = vsync;
            s_steps = static_cast<int>(std::min<int64_t>(s_carryNs / kStepNs, kMaxSteps));
            s_carryNs = std::min(s_carryNs - s_steps * kStepNs, kStepNs);
            s_between = scheduler.vblankPeriod().count() < kStepNs;
            s_since = static_cast<float>(s_carryNs) / static_cast<float>(kStepNs);
        }

        // Runs `original` once per step: all but the last through
        // callGuestFunction with the arguments put back, the last as a tail
        // call, so a yield in it resumes as it would unhooked.
        struct Args
        {
            uint32_t a[4];
            float f12, f13;

            explicit Args(const R5900Context *ctx)
                : a{GPR_U32(ctx, 4), GPR_U32(ctx, 5), GPR_U32(ctx, 6), GPR_U32(ctx, 7)}, f12(ctx->f[12]),
                  f13(ctx->f[13])
            {
            }

            void restore(R5900Context *ctx) const
            {
                for (int i = 0; i < 4; ++i)
                    SET_GPR_U32(ctx, 4 + i, a[i]);
                ctx->f[12] = f12;
                ctx->f[13] = f13;
            }
        };

        void runSteps(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t address,
                      PS2Runtime::RecompiledFunction original)
        {
            const Args args(ctx);
            for (int i = 1; i < s_steps; ++i)
            {
                runtime->callGuestFunction(rdram, ctx, address, {args.a[0], args.a[1], args.a[2], args.a[3]},
                                           original);
                args.restore(ctx);
            }
            original(rdram, ctx, runtime);
        }

        void returnNow(R5900Context *ctx)
        {
            ctx->pc = GPR_U32(ctx, 31);
        }

        // CharHair: strands from +0x2c to +0x30, 0x90 each, Poll doing
        // nothing without +0x44 (0x176ffc-0x177010). A strand: root at +0x8,
        // points from +0x10 to +0x14, 0x70 each, a point's bone at +0x48
        // (0x1770c8, 0x177204, 0x1778ec).
        bool posing(uint8_t *rdram, uint32_t hair)
        {
            return load<uint32_t>(rdram, hair + 0x2cu) != load<uint32_t>(rdram, hair + 0x30u) &&
                   load<uint32_t>(rdram, hair + 0x44u) != 0u;
        }

        template <typename Visit>
        void forEachStrand(uint8_t *rdram, uint32_t hair, Visit visit)
        {
            const uint32_t strandsEnd = load<uint32_t>(rdram, hair + 0x30u);
            for (uint32_t strand = load<uint32_t>(rdram, hair + 0x2cu); strand < strandsEnd; strand += 0x90u)
            {
                const uint32_t root = load<uint32_t>(rdram, strand + 0x8u);
                if (root == 0u)
                    continue;
                std::vector<uint32_t> bones;
                const uint32_t pointsEnd = load<uint32_t>(rdram, strand + 0x14u);
                for (uint32_t point = load<uint32_t>(rdram, strand + 0x10u); point < pointsEnd; point += 0x70u)
                    bones.push_back(load<uint32_t>(rdram, point + 0x48u));
                visit(root, bones);
            }
        }

        using Vec3 = std::array<float, 3>;

        Matrix worldXfm(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t transformable)
        {
            return readTransform(
                rdram, static_cast<uint32_t>(runtime->callGuestFunction(rdram, ctx, s_addresses->worldXfm,
                                                                        {transformable})));
        }

        struct Pose
        {
            std::vector<Vec3> roots;
            std::vector<uint32_t> bones;
            std::vector<Matrix> worlds;
        };

        struct LastSteps
        {
            Pose before, last;
        };
        std::unordered_map<uint32_t, LastSteps> s_hairSteps;

        Pose capture(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t hair)
        {
            Pose pose;
            forEachStrand(rdram, hair, [&](uint32_t root, const std::vector<uint32_t> &bones) {
                const Matrix world = worldXfm(rdram, ctx, runtime, root);
                pose.roots.push_back({world[12], world[13], world[14]});
                for (uint32_t bone : bones)
                {
                    pose.bones.push_back(bone);
                    pose.worlds.push_back(readTransform(rdram, bone + milo::transformable::kWorld));
                }
            });
            return pose;
        }

        Vec3 cross(const Vec3 &a, const Vec3 &b)
        {
            return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
        }

        bool normalize(Vec3 &v)
        {
            const float length = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
            if (length < 1e-6f)
                return false;
            for (float &c : v)
                c /= length;
            return true;
        }

        // Poll's bones are orthonormal, z = x cross y (0x177874-0x1778e0).
        Matrix blend(const Matrix &a, const Matrix &b, float t)
        {
            const auto mix = [&](int i) { return a[i] + (b[i] - a[i]) * t; };
            Vec3 x{mix(0), mix(1), mix(2)};
            Vec3 y{mix(4), mix(5), mix(6)};
            if (!normalize(y))
                return b;
            Vec3 z = cross(x, y);
            if (!normalize(z))
                return b;
            x = cross(y, z);
            Matrix out{};
            for (int c = 0; c < 3; ++c)
            {
                out[c] = x[c];
                out[4 + c] = y[c];
                out[8 + c] = z[c];
                out[12 + c] = mix(12 + c);
            }
            out[15] = 1.0f;
            return out;
        }

        void show(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t hair, const LastSteps &steps,
                  float t)
        {
            const Pose &a = steps.before;
            const Pose &b = steps.last;
            // The Transform SetWorldXfm copies from, on the guest stack.
            const uint32_t sp = GPR_U32(ctx, 29);
            const uint32_t xfm = sp - 0x40u;
            SET_GPR_U32(ctx, 29, xfm);
            size_t strand = 0;
            size_t point = 0;
            forEachStrand(rdram, hair, [&](uint32_t root, const std::vector<uint32_t> &bones) {
                if (strand >= b.roots.size() || point + bones.size() > b.bones.size() ||
                    !std::equal(bones.begin(), bones.end(), b.bones.begin() + point))
                    return;
                const Matrix now = worldXfm(rdram, ctx, runtime, root);
                Vec3 offset;
                for (int c = 0; c < 3; ++c)
                    offset[c] = now[12 + c] - (a.roots[strand][c] + (b.roots[strand][c] - a.roots[strand][c]) * t);
                ++strand;
                for (uint32_t bone : bones)
                {
                    Matrix world = blend(a.worlds[point], b.worlds[point], t);
                    ++point;
                    for (int c = 0; c < 3; ++c)
                        world[12 + c] += offset[c];
                    for (uint32_t row = 0; row < 4; ++row)
                        for (uint32_t col = 0; col < 4; ++col)
                            store<float>(rdram, xfm + row * 16u + col * 4u, col < 3 ? world[row * 4 + col] : 0.0f);
                    runtime->callGuestFunction(rdram, ctx, s_addresses->setWorldXfm, {bone, xfm});
                }
            });
            SET_GPR_U32(ctx, 29, sp);
        }

        // callGuestFunction never yields, so the steps finish before the pose
        // is read.
        PS2Runtime::RecompiledFunction s_hairPoll = nullptr;
        void hairPoll(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t hair = GPR_U32(ctx, 4);
            for (int i = 0; i < s_steps; ++i)
                runtime->callGuestFunction(rdram, ctx, s_addresses->charHairPoll, {hair}, s_hairPoll);
            if (!s_between || !posing(rdram, hair))
            {
                s_hairSteps.erase(hair);
                return returnNow(ctx);
            }
            LastSteps &steps = s_hairSteps[hair];
            if (s_steps > 0)
            {
                steps.before = std::move(steps.last);
                steps.last = capture(rdram, ctx, runtime, hair);
                if (steps.before.bones != steps.last.bones)
                    steps.before = steps.last;
            }
            if (!steps.last.bones.empty())
                show(rdram, ctx, runtime, hair, steps, s_since);
            returnNow(ctx);
        }

        // Shake(this, freq, amp, const Vector2 &, Vector3 &pos, Vector3 &rot)
        // ends by copying its spring's position (+0xa0) and angle (+0xb0) to
        // the two outputs; a frame without a step copies them unmoved.
        PS2Runtime::RecompiledFunction s_shake = nullptr;
        void shake(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (s_steps == 0)
            {
                const uint32_t shot = GPR_U32(ctx, 4);
                std::memcpy(getMemPtr(rdram, GPR_U32(ctx, 6)), getMemPtr(rdram, shot + 0xa0u), 16);
                std::memcpy(getMemPtr(rdram, GPR_U32(ctx, 7)), getMemPtr(rdram, shot + 0xb0u), 16);
                return returnNow(ctx);
            }
            runSteps(rdram, ctx, runtime, s_addresses->camShotShake, s_shake);
        }

        // DrawFlare(bool test) moves +0x128 one step toward shown (+1) or
        // hidden (-1), clamped to [0, +0x124], before it draws: shown when
        // not testing, or when the test ran (+0x104) and passed (+0x100 == 0)
        // (0x1f9378-0x1f93cc). Preset so that one step lands where s_steps
        // would.
        struct FlareTag;
        void onDrawFlare(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            if (s_steps == 1)
                return;
            const uint32_t flare = GPR_U32(ctx, 4);
            const bool test = GPR_U32(ctx, 5) != 0u;
            const bool shown = !test || (load<uint32_t>(rdram, flare + 0x104u) != 0u &&
                                         load<uint32_t>(rdram, flare + 0x100u) == 0u);
            const int32_t dir = shown ? 1 : -1;
            const int32_t steps = load<int32_t>(rdram, flare + 0x124u);
            const int32_t at = load<int32_t>(rdram, flare + 0x128u);
            const int32_t target = std::clamp(at + dir * s_steps, 0, std::max(steps, 0));
            store<int32_t>(rdram, flare + 0x128u, target - dir);
        }
    }

    void installFrameStep(PS2Runtime &runtime, const Addresses &addresses)
    {
        s_addresses = &addresses;
        s_lastVSync = runtime.eeScheduler().currentVSyncTick();
        EntryHook<EndDrawingTag>::install(runtime, addresses.psRndEndDrawing, onEndDrawing);
        s_hairPoll = runtime.lookupFunction(addresses.charHairPoll);
        runtime.replaceFunction(addresses.charHairPoll, &hairPoll);
        s_shake = runtime.lookupFunction(addresses.camShotShake);
        runtime.replaceFunction(addresses.camShotShake, &shake);
        EntryHook<FlareTag>::install(runtime, addresses.rndFlareDrawFlare, onDrawFlare);
    }
}
