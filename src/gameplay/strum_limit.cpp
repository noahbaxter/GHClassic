// strum_limit off: a strum is dropped only when another was taken that frame.
// Retail drops one within 52 ms of the last taken (BeatMatcher::Swing,
// 0x2356e4), checked against the frame's song time, so at 60 frames a second
// only every fourth frame takes one: 15 strums a second, and 16.7 at PAL's 50.
// On, this isn't installed and retail's check stands.

#include "gameplay/strum_limit.h"

#include "guest/beatmatch.h"
#include "hook.h"
#include "ps2_runtime_macros.h"
#include "settings/settings.h"

namespace gh2
{
    namespace
    {
        constexpr float kRetailGapMs = 52.0f;

        // BeatMatcher::Swing(slot, isStrum), retail 0x2356b8: drops a strum
        // when now <= lastSwing + 52, else sets lastSwing to now and hands it
        // to the track's watcher.
        struct SwingTag;
        bool onSwing(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            if (GPR_U32(ctx, 6) == 0u)
                return false;
            const guest::BeatMatcher matcher(rdram, GPR_U32(ctx, 4));
            if (matcher.now() <= matcher.lastSwing())
            {
                leave(ctx);
                return true;
            }
            // Far enough back that retail's own check takes it.
            matcher.setLastSwing(matcher.now() - kRetailGapMs - 1.0f);
            return false;
        }
    }

    void installStrumLimit(PS2Runtime &runtime, const Addresses &addresses)
    {
        if (!settings::get(settings::kStrumLimit))
            Override<SwingTag>::install(runtime, addresses.beatMatcherSwing, onSwing);
    }
}
