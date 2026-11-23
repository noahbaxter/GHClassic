#include "gameplay/engine/no_fail.h"

#include "content/crowd_presence.h"
#include "content/locale.h"
#include "dev/timing.h"
#include "dta.h"
#include "guest.h"
#include "hook.h"
#include "ps2_runtime.h"
#include "script.h"

#include "ps2_runtime_macros.h"

#include <cstdio>
#include <cstring>
#include <iostream>

namespace gh2
{
    namespace
    {
        // SideRailAnim's types: the warning's red, and it with star power's.
        constexpr uint32_t kRailWarning = 1u, kRailWarningStar = 3u;
        // Their frame in beats, a beat long: grey on the beat, reddest
        // three quarters through it.
        constexpr float kRailRed = 0.75f;

        const Addresses *s_addresses = nullptr;
        bool s_on = false; // the modifier, ticked until the game closes
        bool s_failed = false;

        // {no_fail}, whether it's ticked; {no_fail <0|1>} ticks it.
        script::Node noFailCommand(const script::Call &call)
        {
            if (call.size() > 1)
                s_on = call.number(1) != 0.0f;
            return {s_on ? 1u : 0u, script::kInt};
        }

        bool quickplay(uint8_t *rdram)
        {
            const uint32_t config = load<uint32_t>(rdram, s_addresses->theGameConfig);
            const uint32_t mode = config ? load<uint32_t>(rdram, config + 0x3cu) : 0u;
            return mode != 0u && std::strcmp(reinterpret_cast<const char *>(getMemPtr(rdram, mode)), "quickplay") == 0;
        }

        // Performer::LoseGame(this), retail 0x10fe30: GamePanel::SetGameOver(false).
        struct LoseTag;
        bool onLose(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (!s_on || !quickplay(rdram))
                return false;
            if (!s_failed)
            {
                char line[80];
                std::snprintf(line, sizeof line, "[song] failed at %.1f s, played on (no_fail)",
                              timing::songSeconds());
                std::cerr << line << std::endl;
                const R5900Context saved = *ctx;
                script::run(rdram, ctx, runtime, "{no_fail_meter_hold}");
                *ctx = saved;
                crowd_presence::setCrowd(rdram, ctx, runtime, false);
            }
            s_failed = true;
            leave(ctx);
            return true;
        }

        // CrowdMeter::Poll: after a fail the meter left off, as it died.
        struct MeterPollTag;
        bool onMeterPoll(uint8_t *, R5900Context *ctx, PS2Runtime *)
        {
            if (!s_failed)
                return false;
            leave(ctx);
            return true;
        }

        // GamePanel::Exit: a crowd that left stays gone while the venue fades
        // out, its loop off for the menus; the next song's Reset (s_failed
        // still set) brings it back. The run stays marked failed for its
        // results.
        struct ExitTag;
        void onExit(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            crowd_presence::leaveVenue(rdram, ctx, runtime);
        }

        // SideRailAnim::GetType(PlayerState &): after a fail the highway's
        // edges the warning's red, star power's with it while it is on.
        struct RailTypeTag;
        bool onRailType(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            if (!s_failed)
                return false;
            const bool star = load<uint32_t>(rdram, GPR_U32(ctx, 5) + 4u) != 0u;
            SET_GPR_U32(ctx, 2, star ? kRailWarningStar : kRailWarning);
            leave(ctx);
            return true;
        }

        // SideRailAnim::Poll(seconds, PlayerState &, PlayerState &), retail
        // 0x15e158: its type's anim (this +0x20, the anims at [this]) set to
        // the song's beat, so the warning flashes with it; after a fail held
        // on its reddest.
        struct RailPollTag;
        bool onRailPoll(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (!s_failed)
                return false;
            const uint32_t self = GPR_U32(ctx, 4);
            const R5900Context saved = *ctx;
            runtime->callGuestFunction(rdram, ctx, s_addresses->sideRailAnimGetType, {self, GPR_U32(ctx, 5)});
            const uint32_t type = GPR_U32(ctx, 2);
            store<uint32_t>(rdram, self + 0x20u, type);
            // Anim::SetFrame(frame, blend), its vtable's +0x1c past the
            // Anim base's offset at +0x18.
            const uint32_t anim = load<uint32_t>(rdram, load<uint32_t>(rdram, self) + type * 4u);
            const uint32_t vtable = load<uint32_t>(rdram, anim + 0xcu);
            ctx->f[12] = kRailRed;
            ctx->f[13] = 1.0f;
            runtime->callGuestFunction(rdram, ctx, load<uint32_t>(rdram, vtable + 0x1cu),
                                       {anim + static_cast<uint32_t>(load<int16_t>(rdram, vtable + 0x18u))});
            *ctx = saved;
            leave(ctx);
            return true;
        }

        // TrackPanel::Reset, as a song starts and on each restart: the meter
        // on again and the crowd back.
        struct ResetTag;
        void onReset(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (!s_failed)
                return;
            s_failed = false;
            crowd_presence::setCrowd(rdram, ctx, runtime, true);
            const EntryArgs args(ctx);
            script::run(rdram, ctx, runtime, "{no_fail_meter_back}");
            args.restore(ctx);
        }
    }

    void installNoFail(PS2Runtime &runtime, const Addresses &addresses)
    {
        s_addresses = &addresses;
        script::runWhenUiReady(kNoFailDta);
        script::addCommand("no_fail", noFailCommand);
        locale::add("ghc_no_fail", "No Fail");
        crowd_presence::install(runtime, addresses);
        Override<LoseTag>::install(runtime, addresses.performerLoseGame, onLose);
        Override<MeterPollTag>::install(runtime, addresses.crowdMeterPoll, onMeterPoll);
        Override<RailTypeTag>::install(runtime, addresses.sideRailAnimGetType, onRailType);
        Override<RailPollTag>::install(runtime, addresses.sideRailAnimPoll, onRailPoll);
        EntryHook<ExitTag>::install(runtime, addresses.gamePanelExit, onExit);
        EntryHook<ResetTag>::install(runtime, addresses.trackPanelReset, onReset);
    }

    bool failedThisPlay()
    {
        return s_failed;
    }
}
