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

#include "frame_step.h"

#include "guest.h"
#include "hook.h"
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "runtime/ee_scheduler.h"

#include <algorithm>
#include <chrono>
#include <cstring>

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
        int s_steps = 1; // for the frame being built

        struct EndDrawingTag;
        void onEndDrawing(uint8_t *, R5900Context *, PS2Runtime *runtime)
        {
            const EeScheduler &scheduler = runtime->eeScheduler();
            const uint64_t vsync = scheduler.currentVSyncTick();
            s_carryNs += static_cast<int64_t>(vsync - s_lastVSync) * scheduler.vblankPeriod().count();
            s_lastVSync = vsync;
            s_steps = static_cast<int>(std::min<int64_t>(s_carryNs / kStepNs, kMaxSteps));
            s_carryNs = std::min(s_carryNs - s_steps * kStepNs, kStepNs);
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

        PS2Runtime::RecompiledFunction s_hairPoll = nullptr;
        void hairPoll(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (s_steps == 0)
                return returnNow(ctx);
            runSteps(rdram, ctx, runtime, s_addresses->charHairPoll, s_hairPoll);
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
