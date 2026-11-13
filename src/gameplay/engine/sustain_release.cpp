// sustain_release_ms: a sustain let go this close to its end leaves the
// track heard, as one held to its end does. Scoring takes its own sink
// (Player::ReleaseGem, retail 0x111810) and is as retail's.
//
// miss_mute off: no miss or sustain let go early cuts the track. The game
// mutes it through MasterAudio::MuteTrack from a miss (DontPlay), a release
// (ReleaseGem) and each gem coming while the player is not hitting
// (SeeGem, IsSucceeding); MuteAllTracks, at a song's start, mutes as ever.
// {miss_mute [<on>]} is it, set to that with one (Audio Settings' box).

#include "gameplay/engine/sustain_release.h"

#include "hook.h"
#include "script.h"
#include "settings/settings.h"

namespace gh2
{
    namespace
    {
        PS2Runtime::RecompiledFunction s_muteTrack = nullptr;

        struct ReleaseGemTag;

        // MasterAudio::ReleaseGem(track, player, gem, ms left), retail
        // 0x239ec8: mutes the track when any is left (0x239f40).
        void onReleaseGem(uint8_t *, R5900Context *ctx, PS2Runtime *)
        {
            const float left = ctx->f[12];
            if (left > 0.0f && left <= static_cast<float>(settings::get(settings::kSustainReleaseMs)))
                ctx->f[12] = 0.0f;
        }

        // Where callGuestFunction returns to: the one host caller,
        // track_start.cpp's MuteAllTracks in place of the game's.
        constexpr uint32_t kFromMuteAll = 0xfffffff0u;

        void muteTrack(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (!settings::get(settings::kMissMute) && GPR_U32(ctx, 31) != kFromMuteAll)
            {
                ctx->pc = GPR_U32(ctx, 31);
                return;
            }
            s_muteTrack(rdram, ctx, runtime);
        }
    }

    void installSustainRelease(PS2Runtime &runtime, const Addresses &addresses)
    {
        EntryHook<ReleaseGemTag>::install(runtime, addresses.masterAudioReleaseGem, onReleaseGem);
        replaceKeeping(runtime, addresses.muteTrack, &muteTrack, s_muteTrack);
        script::addCommand("miss_mute", [](const script::Call &call) -> script::Node
                           {
                               if (call.size() > 1)
                                   settings::set(settings::kMissMute, call.number(1) != 0.0f ? 1 : 0);
                               return {settings::get(settings::kMissMute) ? 1u : 0u, script::kInt};
                           });
    }
}
