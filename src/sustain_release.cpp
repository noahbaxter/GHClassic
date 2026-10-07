// sustain_release_ms: a sustain let go this close to its end leaves the
// track heard, as one held to its end does. Scoring takes its own sink
// (Player::ReleaseGem, retail 0x111810) and is as retail's.

#include "sustain_release.h"

#include "hook.h"
#include "settings/settings.h"

namespace gh2
{
    namespace
    {
        struct ReleaseGemTag;

        // MasterAudio::ReleaseGem(track, player, gem, ms left), retail
        // 0x239ec8: mutes the track when any is left (0x239f40).
        void onReleaseGem(uint8_t *, R5900Context *ctx, PS2Runtime *)
        {
            const float left = ctx->f[12];
            if (left > 0.0f && left <= static_cast<float>(settings::get(settings::kSustainReleaseMs)))
                ctx->f[12] = 0.0f;
        }
    }

    void installSustainRelease(PS2Runtime &runtime, const Addresses &addresses)
    {
        EntryHook<ReleaseGemTag>::install(runtime, addresses.masterAudioReleaseGem, onReleaseGem);
    }
}
