// strum_limit off: a strum is dropped only when another was taken that frame.
// Retail drops one within 52 ms of the last taken (BeatMatcher::Swing,
// 0x2356e4), checked against the frame's song time, so at 60 frames a second
// only every fourth frame takes one: 15 strums a second, and 16.7 at PAL's 50.
// On, this isn't installed and retail's check stands.

#include "strum_limit.h"

#include "guest.h"
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "settings/settings.h"

namespace gh2
{
    namespace
    {
        constexpr uint32_t kNow = 0xa4u;       // BeatMatcher: the song's ms this frame
        constexpr uint32_t kLastSwing = 0xc4u; // and when it last took a strum

        constexpr float kRetailGapMs = 52.0f;

        PS2Runtime::RecompiledFunction s_swing = nullptr;

        // BeatMatcher::Swing(slot, isStrum), retail 0x2356b8: drops a strum
        // when now <= lastSwing + 52, else sets lastSwing to now and hands it
        // to the track's watcher.
        void swing(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t matcher = GPR_U32(ctx, 4);
            const bool isStrum = GPR_U32(ctx, 6) != 0u;
            if (isStrum)
            {
                const float now = load<float>(rdram, matcher + kNow);
                if (now <= load<float>(rdram, matcher + kLastSwing))
                {
                    ctx->pc = GPR_U32(ctx, 31);
                    return;
                }
                // Far enough back that retail's own check takes it.
                store<float>(rdram, matcher + kLastSwing, now - kRetailGapMs - 1.0f);
            }
            s_swing(rdram, ctx, runtime);
        }
    }

    void installStrumLimit(PS2Runtime &runtime, const Addresses &addresses)
    {
        if (settings::get(settings::kStrumLimit))
            return;
        s_swing = runtime.lookupFunction(addresses.beatMatcherSwing);
        runtime.replaceFunction(addresses.beatMatcherSwing, &swing);
    }
}
