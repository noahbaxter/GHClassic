// track_on_at_start: a player's track heard from the song's start, as if
// the note before its first had been hit, rather than silent until a hit.

#include "track_start.h"

#include "guest.h"
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "settings/settings.h"

namespace gh2
{
    namespace
    {
        const Addresses *s_addresses = nullptr;

        // MasterAudio::MuteAllTracks(fade), retail 0x23a3e8, called only by
        // MasterAudio::Jump (0x23971c) at a song's start: MuteTrack(i, 0, -1,
        // fade) on every track, which Hit undoes with UnmuteTrack (0x239e2c).
        // With the setting, a track a player holds (TrackData +0x34 not -1,
        // MuteTrack's own test at 0x23a304) is unmuted instead.
        void muteAllTracks(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t returnTo = GPR_U32(ctx, 31);
            const uint32_t self = GPR_U32(ctx, 4);
            const float fade = ctx->f[12];
            const bool on = settings::get(settings::kTrackOnAtStart) != 0;
            const uint32_t begin = load<uint32_t>(rdram, self + 0x3cu); // mTrackData
            const uint32_t end = load<uint32_t>(rdram, self + 0x40u);
            for (uint32_t i = 0; begin + i * 4u < end; ++i)
            {
                const uint32_t track = load<uint32_t>(rdram, begin + i * 4u);
                if (on && load<int32_t>(rdram, track + 0x34u) != -1)
                {
                    runtime->callGuestFunction(rdram, ctx, s_addresses->unmuteTrack, {self, i, 0u, ~0u});
                    continue;
                }
                ctx->f[12] = fade;
                runtime->callGuestFunction(rdram, ctx, s_addresses->muteTrack, {self, i, 0u, ~0u});
            }
            ctx->pc = returnTo;
        }
    }

    void installTrackStart(PS2Runtime &runtime, const Addresses &addresses)
    {
        s_addresses = &addresses;
        runtime.replaceFunction(addresses.muteAllTracks, muteAllTracks);
    }
}
