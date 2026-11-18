// A NowBar (one a track) draws its hit effects with SlotViews (+0x20): at
// each fret a group (+0x50, by fret) placed on that fret's smasher, there
// until reset. A hit puts its SmashFX's group (+0x14, its group +0x04) at the
// gem's frets and resets the rest (NowBar::Hit, 0x15d0b8); the group holds
// the burst and, for a gem with a sustain, the burn (its BurnFX, +0x18). So
// one burn, on the last gem hit. Here a hit while the SmashFX still burns
// for a fret held takes another SmashFX (made as NowBar's, 0x15cb2c, kept
// for again), the burning one left where it is, its frets not reset; each
// fret reset once no sustain holds it. No flame moves or starts again.

#include "highway/extended/guitar/sustain_flames.h"

#include "gameplay/extended/guitar/gem_kinds.h"
#include "gameplay/extended/guitar/hammer_ons.h"
#include "gameplay/extended/guitar/sustain_hold.h"
#include "guest.h"
#include "guest/beatmatch.h"
#include "highway/gem_looks.h"
#include "hook.h"

#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"

#include <algorithm>
#include <map>
#include <vector>

namespace gh2::guitar
{
    namespace
    {
        const Addresses *s_addresses = nullptr;

        constexpr uint32_t kNowBarSmash = 0x14u, kNowBarViews = 0x20u;
        constexpr uint32_t kSmashGroup = 0x04u, kSmashSize = 0x1cu;
        constexpr uint32_t kDelete = 3u;
        struct Bar
        {
            uint32_t own = 0u;                    // NowBar's own SmashFX
            std::vector<uint32_t> spare;          // ours, burning none
            std::map<uint32_t, uint8_t> burning;  // each SmashFX burning, its frets
            bool bonus = false;                   // star power's, as SetBonus last had it
        };
        std::map<uint32_t, Bar> s_bars; // by NowBar
        uint32_t s_hitViews = 0u;      // the SlotViews of the NowBar hitting, 0 for none
        uint8_t s_hitKeep = 0u;        // the frets it leaves burning

        uint32_t watcherOf(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t nowBar)
        {
            return trackWatcher(trackNum(rdram, ctx, runtime, load<uint32_t>(rdram, nowBar + nowBar::kConfig)));
        }

        uint32_t shownAt(uint8_t *rdram, uint32_t views, uint32_t lane)
        {
            return load<uint32_t>(rdram, load<uint32_t>(rdram, views + slotViews::kViews) + lane * 4u);
        }

        // A SmashFX for the NowBar's next hit: a spare, or one made.
        uint32_t spareSmash(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t nowBar, Bar &bar)
        {
            if (!bar.spare.empty())
            {
                const uint32_t smash = bar.spare.back();
                bar.spare.pop_back();
                return smash;
            }
            const uint32_t players = static_cast<uint32_t>(runtime->callGuestFunction(
                rdram, ctx, s_addresses->trackConfigPlayer, {load<uint32_t>(rdram, nowBar + nowBar::kConfig)}));
            const uint32_t smash =
                static_cast<uint32_t>(runtime->callGuestFunction(rdram, ctx, s_addresses->builtinNew, {kSmashSize}));
            runtime->callGuestFunction(rdram, ctx, s_addresses->smashFxCtor, {smash, players > 0u ? 1u : 0u});
            runtime->callGuestFunction(rdram, ctx, s_addresses->smashFxSetBonus, {smash, bar.bonus ? 1u : 0u});
            return smash;
        }

        // NowBar::Hit(float ms, int gem, bool, PhraseState).
        PS2Runtime::RecompiledFunction s_nowBarHit = nullptr;
        void nowBarHit(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (!playingCustom())
                return s_nowBarHit(rdram, ctx, runtime);
            const EntryArgs args(ctx);
            const float f14 = ctx->f[14];
            const uint32_t nowBar = args.a[0];
            Bar &bar = s_bars[nowBar];
            const uint32_t current = load<uint32_t>(rdram, nowBar + kNowBarSmash);
            if (bar.own == 0u)
                bar.own = current;
            // The frets held for sustains before this gem's is taken (HitGem,
            // after), other than its own.
            const uint32_t watcher = watcherOf(rdram, ctx, runtime, nowBar);
            const guest::GameGem game(rdram, watcher ? watcherGem(rdram, watcher, static_cast<int32_t>(args.a[1])) : 0u);
            // A chord hammered on or pulled off to hit at all its frets, not
            // the one it is shown to GH2 as
            // (gameplay/extended/guitar/hammer_ons.h).
            const uint8_t shown = game.address ? game.slots() : 0u;
            const uint8_t slots = game.address ? gemFrets(rdram, game.address) : 0u;
            s_hitKeep = watcher ? heldLanes(rdram, watcher, args.f12) & ~slots : 0u;
            if (const auto burning = bar.burning.find(current); burning != bar.burning.end())
            {
                burning->second &= s_hitKeep;
                if (burning->second != 0u)
                    store<uint32_t>(rdram, nowBar + kNowBarSmash, spareSmash(rdram, ctx, runtime, nowBar, bar));
                else
                    bar.burning.erase(burning);
            }
            s_hitViews = load<uint32_t>(rdram, nowBar + kNowBarViews);
            args.restore(ctx);
            ctx->f[14] = f14;
            if (slots != shown)
                game.setSlots(slots);
            callOriginal(rdram, ctx, runtime, s_addresses->nowBarHit, s_nowBarHit);
            if (slots != shown)
                game.setSlots(shown);
            s_hitViews = 0u;
            if (game.address && !game.noSustain())
                bar.burning[load<uint32_t>(rdram, nowBar + kNowBarSmash)] = slots;
        }

        // SlotViews::Reset(int slot, float), as Hit resets the frets not the
        // gem's: never one burning for a held sustain.
        PS2Runtime::RecompiledFunction s_resetAt = nullptr;
        void resetAt(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t slot = GPR_U32(ctx, 5);
            if (s_hitViews == 0u || GPR_U32(ctx, 4) != s_hitViews || slot >= 8u || !((s_hitKeep >> slot) & 1u))
                return s_resetAt(rdram, ctx, runtime);
            ctx->pc = GPR_U32(ctx, 31);
        }

        // SlotViews::Poll(float ms), each frame (0x15ef30): each fret's group
        // set to its time, then reset once past its length (+0x80, 0 for
        // none). GH2 times every fret from the latest started (+0x60's
        // most), so a hit would start a burn still burning at another fret
        // over; here each is timed from its own start.
        constexpr uint32_t kViewsStarts = 0x60u, kViewsLengths = 0x80u, kViewsRate = 0x3fa05cu;
        PS2Runtime::RecompiledFunction s_viewsPoll = nullptr;
        void viewsPoll(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (!playingCustom())
                return s_viewsPoll(rdram, ctx, runtime);
            const uint32_t views = GPR_U32(ctx, 4);
            const float now = ctx->f[12], rate = load<float>(rdram, kViewsRate);
            for (uint32_t slot = 0; slot < 5u; ++slot)
            {
                const uint32_t group = shownAt(rdram, views, slot);
                if (group == 0u)
                    continue;
                const float at = rate * (now - load<float>(rdram, load<uint32_t>(rdram, views + kViewsStarts) + slot * 4u));
                // Its Animatable's SetFrame(float frame, float blend).
                const uint32_t table = load<uint32_t>(rdram, group + 0x0cu);
                ctx->f[12] = at;
                ctx->f[13] = 1.0f;
                runtime->callGuestFunction(rdram, ctx, load<uint32_t>(rdram, table + 0x1cu),
                                           {group + static_cast<uint32_t>(static_cast<int32_t>(load<int16_t>(rdram, table + 0x18u)))});
                const float length = load<float>(rdram, load<uint32_t>(rdram, views + kViewsLengths) + slot * 4u);
                if (length != 0.0f && length < at)
                    runtime->callGuestFunction(rdram, ctx, s_addresses->slotViewsReset, {views, slot});
            }
            ctx->pc = GPR_U32(ctx, 31);
        }

        // NowBar::Poll(float ms, bool whammy), each frame: each SmashFX
        // burning but NowBar's own whammied as that is, its frets no sustain
        // holds reset, and one burning none put out and kept for again.
        struct NowBarPollTag;
        void onNowBarPoll(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const auto found = s_bars.find(GPR_U32(ctx, 4));
            if (found == s_bars.end() || found->second.burning.empty())
                return;
            const EntryArgs args(ctx);
            Bar &bar = found->second;
            const uint32_t nowBar = args.a[0], views = load<uint32_t>(rdram, nowBar + kNowBarViews);
            const uint32_t current = load<uint32_t>(rdram, nowBar + kNowBarSmash);
            const uint32_t watcher = watcherOf(rdram, ctx, runtime, nowBar);
            const uint8_t held = watcher ? heldLanes(rdram, watcher, args.f12) : 0u;
            for (auto it = bar.burning.begin(); it != bar.burning.end();)
            {
                const uint32_t smash = it->first, group = load<uint32_t>(rdram, smash + kSmashGroup);
                if (smash != current)
                    runtime->callGuestFunction(rdram, ctx, s_addresses->smashFxSetWhammy, {smash, args.a[1]});
                for (uint32_t lane = 0; lane < 5u; ++lane)
                    if (((it->second & ~held) >> lane) & 1u && shownAt(rdram, views, lane) == group)
                        runtime->callGuestFunction(rdram, ctx, s_addresses->slotViewsReset, {views, lane});
                it->second &= held;
                if (it->second != 0u)
                {
                    ++it;
                    continue;
                }
                if (smash != current)
                {
                    runtime->callGuestFunction(rdram, ctx, s_addresses->smashFxRemoveAll, {smash});
                    bar.spare.push_back(smash);
                }
                it = bar.burning.erase(it);
            }
            args.restore(ctx);
        }

        // NowBar::SetBonus(bool), star power on or off: every SmashFX's too.
        struct NowBarBonusTag;
        void onNowBarBonus(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const auto found = s_bars.find(GPR_U32(ctx, 4));
            if (found == s_bars.end())
                return;
            const EntryArgs args(ctx);
            found->second.bonus = args.a[1] != 0u;
            const uint32_t current = load<uint32_t>(rdram, args.a[0] + kNowBarSmash);
            for (const auto &[smash, lanes] : found->second.burning)
                if (smash != current)
                    runtime->callGuestFunction(rdram, ctx, s_addresses->smashFxSetBonus, {smash, args.a[1]});
            args.restore(ctx);
        }

        // ~NowBar: its own SmashFX given back for it to delete, ours deleted.
        struct NowBarGoneTag;
        void onNowBarGone(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const auto found = s_bars.find(GPR_U32(ctx, 4));
            if (found == s_bars.end())
                return;
            const EntryArgs args(ctx);
            const uint32_t nowBar = args.a[0], views = load<uint32_t>(rdram, nowBar + kNowBarViews);
            Bar &bar = found->second;
            std::vector<uint32_t> ours = bar.spare;
            for (const auto &[smash, lanes] : bar.burning)
                ours.push_back(smash);
            ours.push_back(load<uint32_t>(rdram, nowBar + kNowBarSmash));
            store<uint32_t>(rdram, nowBar + kNowBarSmash, bar.own);
            for (uint32_t lane = 0; lane < 5u; ++lane)
                runtime->callGuestFunction(rdram, ctx, s_addresses->slotViewsReset, {views, lane});
            std::sort(ours.begin(), ours.end());
            ours.erase(std::unique(ours.begin(), ours.end()), ours.end());
            for (const uint32_t smash : ours)
                if (smash != bar.own)
                    runtime->callGuestFunction(rdram, ctx, s_addresses->smashFxDtor, {smash, kDelete});
            s_bars.erase(found);
            args.restore(ctx);
        }
    }

    void installSustainFlames(PS2Runtime &runtime, const Addresses &addresses)
    {
        s_addresses = &addresses;
        replaceKeeping(runtime, addresses.nowBarHit, &nowBarHit, s_nowBarHit);
        replaceKeeping(runtime, addresses.slotViewsResetAt, &resetAt, s_resetAt);
        replaceKeeping(runtime, addresses.slotViewsPoll, &viewsPoll, s_viewsPoll);
        EntryHook<NowBarPollTag>::install(runtime, addresses.nowBarPoll, onNowBarPoll);
        EntryHook<NowBarBonusTag>::install(runtime, addresses.nowBarSetBonus, onNowBarBonus);
        EntryHook<NowBarGoneTag>::install(runtime, addresses.nowBarDtor, onNowBarGone);
    }
}
