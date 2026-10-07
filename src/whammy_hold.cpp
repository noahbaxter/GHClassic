// whammy_hold: a whammy bar held down bends every sustain. Retail bends
// only once the bar has been at rest since the last sustain ended.

#include "whammy_hold.h"

#include "guest.h"
#include "hook.h"
#include "ps2_runtime_macros.h"
#include "settings/settings.h"

namespace gh2
{
    namespace
    {
        // TrackWatcherImpl's bend latch: 1 from a poll with the bar at rest
        // (retail 0x248e88), 0 from EndSustainedNote (0x2490ec).
        constexpr uint32_t kBendArmed = 0x68u;
        constexpr uint32_t kArmedHeld = 2u; // ours: armed with the bar never at rest
        constexpr uint32_t kLastBar = 0x114u; // Player: the bar as SetWhammyBar last had it

        bool s_heldSend = false; // SendWhammy's sinks are being told a held bar

        struct PitchBendTag;
        struct WhammyTag;
        struct PlayerWhammyTag;

        // TrackWatcherImpl::CheckForPitchBend, retail 0x248e38: bends only
        // with the latch set (0x248ea0).
        void onCheckForPitchBend(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            const uint32_t latch = GPR_U32(ctx, 4) + kBendArmed;
            if (settings::get(settings::kWhammyHold) && load<uint32_t>(rdram, latch) == 0u)
                store<uint32_t>(rdram, latch, kArmedHeld);
        }

        // TrackWatcherImpl::SendWhammy, retail 0x249b98: the bar to each sink,
        // the player's among them (0x116bb8), within this call.
        void onSendWhammy(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            s_heldSend = load<uint32_t>(rdram, GPR_U32(ctx, 4) + kBendArmed) == kArmedHeld;
        }

        // Player::SetWhammyBar, retail 0x111d88: the bar's speed since the
        // last call earns star power (0x111de8), and the bar itself waves the
        // sustain's tail (GemRep::Poll, 0x158e98). Between sustains it is
        // told 0 (0x248f28), so a held bar's first value would count as a
        // move from rest: it is made the last value too.
        void onPlayerWhammy(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            const uint32_t last = GPR_U32(ctx, 4) + kLastBar;
            if (s_heldSend && ctx->f[12] != 0.0f && load<float>(rdram, last) == 0.0f)
                store<float>(rdram, last, ctx->f[12]);
        }
    }

    void installWhammyHold(PS2Runtime &runtime, const Addresses &addresses)
    {
        EntryHook<PitchBendTag>::install(runtime, addresses.checkForPitchBend, onCheckForPitchBend);
        EntryHook<WhammyTag>::install(runtime, addresses.sendWhammy, onSendWhammy);
        EntryHook<PlayerWhammyTag>::install(runtime, addresses.playerSetWhammyBar, onPlayerWhammy);
    }
}
