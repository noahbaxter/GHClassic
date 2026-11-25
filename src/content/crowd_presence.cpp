#include "content/crowd_presence.h"

#include "dev/timing.h"
#include "dta.h"
#include "guest.h"
#include "hook.h"
#include "milo/layout.h"
#include "ps2_runtime.h"
#include "script.h"

#include "ps2_runtime_macros.h"

#include <algorithm>
#include <cstring>

namespace gh2::crowd_presence
{
    namespace
    {
        // They walk out at the first camera cut once the boo has had this long.
        constexpr float kBooSeconds = 2.0f;
        // Under the boo their loop fades away; once the boo has rung out by
        // itself (about 10.5 s), their audio goes off. Turned off sooner, it
        // takes the boo's tail with it.
        constexpr float kLoopFadeSeconds = 3.0f, kLoopFadeDb = 60.0f, kBooEndSeconds = 11.0f;
        constexpr uint32_t kMasterVolume = 0x2cu; // CrowdAudio's, in dB
        // GH2's ExcitementLevel kExcitementOkay: lights and cameras as at a
        // middling show, not left on the meter's empty.
        constexpr uint32_t kExcitementOkay = 2u;

        const Addresses *s_addresses = nullptr;
        bool s_enabled = true;
        float s_leftAt = 0.0f;      // the song's seconds
        bool s_walkingOut = false;  // waiting on a camera cut
        bool s_gone = false;        // not drawn, nothing new heard
        bool s_fading = false;      // the loop going, the boo ringing out
        bool s_audioOff = false;
        uint32_t s_audio = 0u;      // the CrowdAudio, from its last one-off
        float s_volume = 0.0f;      // its volume before the fade

        // CrowdAudio::SetMasterVolume(dB), which moves its loop alone.
        void setLoopVolume(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, float db)
        {
            const EntryArgs args(ctx);
            ctx->f[12] = db;
            runtime->callGuestFunction(rdram, ctx, s_addresses->crowdAudioSetMasterVolume, {s_audio});
            args.restore(ctx);
        }

        // CrowdAudio::SetExcitement(int): no change of mood, whose one-off
        // would cut the boo off.
        struct MoodTag;
        bool onMood(uint8_t *, R5900Context *ctx, PS2Runtime *)
        {
            if (s_enabled)
                return false;
            leave(ctx);
            return true;
        }

        // CrowdAudio::PlayOneOff(Sfx *): nothing new once they've gone; the
        // boo already playing rings out.
        struct OneOffTag;
        bool onOneOff(uint8_t *, R5900Context *ctx, PS2Runtime *)
        {
            s_audio = GPR_U32(ctx, 4);
            if (!s_gone)
                return false;
            leave(ctx);
            return true;
        }

        // Sequence::Play: star power's claps (game.dta's beat handler, a
        // task 0.9 beats on, so some may be queued) and the groan at a blown
        // big note.
        struct SequenceTag;
        bool onSequence(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            if (s_enabled)
                return false;
            const uint32_t seq = GPR_U32(ctx, 4);
            uint32_t name = load<uint32_t>(rdram, seq + ::milo::object::kName);
            if (name == 0u || name >= 0x2000000u)
                name = load<uint32_t>(rdram, load<uint32_t>(rdram, seq) + ::milo::object::kName);
            if (name == 0u || name >= 0x2000000u)
                return false;
            const char *const text = reinterpret_cast<const char *>(rdram + name);
            if (std::strncmp(text, "claps", 5) != 0 && std::strncmp(text, "blew_bignote", 12) != 0)
                return false;
            leave(ctx);
            return true;
        }

        // CamShot::StartAnim, as a shot starts: they're gone.
        struct CutTag;
        void onCut(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (!s_walkingOut || timing::songSeconds() - s_leftAt < kBooSeconds)
                return;
            s_walkingOut = false;
            s_gone = true;
            const R5900Context saved = *ctx;
            script::run(rdram, ctx, runtime, "{crowd_presence_hide}");
            *ctx = saved;
        }

        // Times the crowd drew since {crowd_presence drawn} last asked: for
        // a test to see them there or gone, whatever the camera shows.
        uint32_t s_drawn = 0u;

        // WorldCrowd::DrawShowing(DrawType).
        struct DrawTag;
        bool onDraw(uint8_t *, R5900Context *ctx, PS2Runtime *)
        {
            if (!s_gone)
            {
                ++s_drawn;
                return false;
            }
            leave(ctx);
            return true;
        }

        script::Node command(const script::Call &call)
        {
            if (call.symbol(1) == "drawn")
            {
                const uint32_t drawn = s_drawn;
                s_drawn = 0u;
                return {drawn, script::kInt};
            }
            if (call.symbol(1) == "gone")
                return {s_gone ? 1u : 0u, script::kInt};
            return {};
        }

        // GamePanel::SetExcitementLevel(level), each frame from its Poll.
        struct ExcitementTag;
        void onExcitement(uint8_t *, R5900Context *ctx, PS2Runtime *)
        {
            if (!s_enabled)
                SET_GPR_U32(ctx, 5, kExcitementOkay);
        }

        // Each frame of a song (BandScoreDisplay::SetScore): the loop faded
        // under the boo, then, the boo done, their audio off.
        struct FrameTag;
        void onFrame(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (!s_fading)
                return;
            const float t = timing::songSeconds() - s_leftAt;
            if (t < kBooEndSeconds)
            {
                setLoopVolume(rdram, ctx, runtime, s_volume - kLoopFadeDb * std::min(t / kLoopFadeSeconds, 1.0f));
                return;
            }
            s_fading = false;
            s_audioOff = true;
            const EntryArgs args(ctx);
            script::run(rdram, ctx, runtime, "{crowd_presence_mute}");
            args.restore(ctx);
        }
    }

    bool setCrowd(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, bool enabled)
    {
        if (enabled == s_enabled)
            return false;
        s_enabled = enabled;
        const R5900Context saved = *ctx;
        if (!enabled)
        {
            s_leftAt = timing::songSeconds();
            s_walkingOut = true;
            // CrowdAudio's game_lost plays the venue's crowd_lose.
            script::run(rdram, ctx, runtime, "{crowd_audio game_lost}");
            if (s_audio)
            {
                s_volume = load<float>(rdram, s_audio + kMasterVolume);
                s_fading = true;
            }
        }
        else
        {
            if (s_gone)
                script::run(rdram, ctx, runtime, "{crowd_presence_show}");
            if (s_fading || s_audioOff)
                setLoopVolume(rdram, ctx, runtime, s_volume);
            if (s_audioOff)
                script::run(rdram, ctx, runtime, "{crowd_presence_unmute}");
            s_walkingOut = s_gone = s_fading = s_audioOff = false;
        }
        *ctx = saved;
        return true;
    }

    bool leaveVenue(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (s_enabled)
            return false;
        const R5900Context saved = *ctx;
        // Their sound back as it was, here in the song's mode, then off for
        // the menus; only the picture waits for the next song.
        if (s_fading || s_audioOff)
            setLoopVolume(rdram, ctx, runtime, s_volume);
        if (s_audioOff)
            script::run(rdram, ctx, runtime, "{crowd_presence_unmute}");
        script::run(rdram, ctx, runtime, "{crowd_audio turn_off}");
        s_walkingOut = s_fading = s_audioOff = false;
        *ctx = saved;
        return true;
    }

    void install(PS2Runtime &runtime, const Addresses &addresses)
    {
        s_addresses = &addresses;
        script::runWhenUiReady(kCrowdPresenceDta);
        script::addCommand("crowd_presence", command);
        Override<MoodTag>::install(runtime, addresses.crowdAudioSetExcitement, onMood);
        Override<OneOffTag>::install(runtime, addresses.crowdAudioPlayOneOff, onOneOff);
        Override<SequenceTag>::install(runtime, addresses.sequencePlay, onSequence);
        EntryHook<CutTag>::install(runtime, addresses.camShotStartAnim, onCut);
        Override<DrawTag>::install(runtime, addresses.worldCrowdDrawShowing, onDraw);
        EntryHook<ExcitementTag>::install(runtime, addresses.gamePanelSetExcitementLevel, onExcitement);
        EntryHook<FrameTag>::install(runtime, addresses.bandScoreDisplaySetScore, onFrame);
    }
}
