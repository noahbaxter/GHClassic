#include "gameplay/extended/guitar/hammer_ons.h"

#include "gameplay/extended/guitar/gem_kinds.h"
#include "gameplay/extended/guitar/sustain_hold.h"
#include "guest.h"
#include "hook.h"

#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"

#include <algorithm>
#include <iterator>

namespace gh2::guitar
{
    namespace
    {
        using guest::GameGem;
        using guest::GuitarTrackWatcherImpl;

        const Addresses *s_addresses = nullptr;

        // A chord being hammered on or pulled off to (hammerChord), shown to
        // GH2 as its highest fret alone for the call, and its own frets.
        uint32_t s_chordGem = 0u;
        uint8_t s_chordFrets = 0u;
        // The open HOPO hammered on, shown as kOpenLane's.
        uint32_t s_hammeredOpen = 0u;

        // An open scores as one fret: a gem's points, its sustain's and the
        // song's most to score (for its stars) go by how many frets it has,
        // each asked where it is scored. Elsewhere, Swing's matching above
        // all, it has none.
        constexpr uint32_t kScoredAt[] = {
            0x112238u, // Player::AddHeadPoints, GameGem::NumSlots
            0x12a110u, // HeldNote::HeldNote, GameGem::NumSlots
            0x12a5ccu, // MultiplayerAnalyzer::AddMultiGem, GemNumSlots
        };

        // A chord being hammered on scores as its own frets, not the one it
        // is shown as: asked of the gem (GameGem::NumSlots) or of the frets
        // it shows (GemNumSlots).
        template <PS2Runtime::RecompiledFunction *Original>
        void scoredSlots(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t from = GPR_U32(ctx, 31), asked = GPR_U32(ctx, 4);
            (*Original)(rdram, ctx, runtime);
            if (std::find(std::begin(kScoredAt), std::end(kScoredAt), from) == std::end(kScoredAt))
                return;
            if (s_chordGem != 0u && (asked == s_chordGem || asked == load<uint8_t>(rdram, s_chordGem + 0x0cu)))
                SET_GPR_U32(ctx, 2, static_cast<uint32_t>(__builtin_popcount(s_chordFrets)));
            else if (GPR_U32(ctx, 2) == 0u)
                SET_GPR_U32(ctx, 2, 1u);
        }
        PS2Runtime::RecompiledFunction s_numSlots = nullptr, s_gemNumSlots = nullptr;

        // An open HOPO is hit by letting go of the last fret held. GH2 tries
        // a hammer-on or pull-off (GuitarTrackWatcherImpl::TryToHopo,
        // 0x24dc20, by way of NonStrumSwing, 0x24d828) for a fret pressed
        // or one left held, never for none, and matches the gem's frets to
        // that one alone. So with every fret let go and an open HOPO next,
        // it is tried as the middle lane's, that gem one fret for the try.
        //
        // GuitarTrackWatcherImpl: its gems (+0x04, GameGemList, theirs at
        // +0x08, 0x10 each), its clock (+0x0c, a virtual at +0x10 its now in
        // ms) and the frets held (+0x84). GameGem: slots +0x0c, flags +0x0d
        // (bit 0 HOPO).
        constexpr uint32_t kWatcherGems = 0x04u, kWatcherClock = 0x0cu, kWatcherHeld = 0x84u;
        constexpr uint32_t kListGems = 0x08u, kGemSize = 0x10u, kGemSlots = 0x0cu, kGemFlags = 0x0du;
        // The watcher's track at +0x44 (its constructor's first int; the
        // second, at +0x00, is its player's, both 0 for one player); a
        // GameGem's tick at +0x04; which gems were hit, words at +0x04 of the
        // list at +0x08, and the last hit, +0x14.
        constexpr uint32_t kWatcherTrack = 0x44u, kWatcherHits = 0x08u, kWatcherLastHit = 0x14u, kGemTick = 0x04u;

        const Addresses *s_addresses = nullptr;

        bool tapGem(uint8_t *rdram, uint32_t watcher, uint32_t gem)
        {
            return isTap(load<uint32_t>(rdram, watcher + kWatcherTrack), load<uint32_t>(rdram, gem + kGemTick),
                         load<uint8_t>(rdram, gem + kGemSlots));
        }
        // GH2's own rule for a HOPO (0x24df30): the gem before it the last
        // hit.
        bool hopoFollows(uint8_t *rdram, uint32_t watcher, int32_t index)
        {
            if (index <= 0)
                return false;
            const uint32_t hits = load<uint32_t>(rdram, load<uint32_t>(rdram, watcher + kWatcherHits) + 0x04u);
            return load<uint32_t>(rdram, hits + static_cast<uint32_t>(index - 1) * 4u) != 0u &&
                   static_cast<int32_t>(load<uint32_t>(rdram, watcher + kWatcherLastHit)) == index - 1;
        }

        // A HOPO or tap chord hammered on once a fret pressed makes the frets
        // held its own (those held for a sustain aside,
        // gameplay/extended/guitar/sustain_hold.h): shown to GH2 as its highest
        // fret alone (the others held below it, which a hammer-on allows) for
        // NonStrumSwing, which times and hits it, scored as its frets
        // (scoredSlots). A tap's whatever is before it, a HOPO's after the last
        // hit.
        void hammerChord(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t watcher, int32_t index)
        {
            const uint32_t gem = watcherGem(rdram, watcher, index);
            const uint8_t slots = load<uint8_t>(rdram, gem + kGemSlots);
            const uint32_t held = load<uint32_t>(rdram, watcher + kWatcherHeld) & ~sustainedLanes(rdram, watcher, slots);
            if (__builtin_popcount(slots) < 2 || !(load<uint8_t>(rdram, gem + kGemFlags) & 1u) || held != slots ||
                !(tapGem(rdram, watcher, gem) || hopoFollows(rdram, watcher, index)))
                return;
            const uint32_t top = 31u - static_cast<uint32_t>(__builtin_clz(slots));
            s_chordGem = gem, s_chordFrets = slots;
            store<uint8_t>(rdram, gem + kGemSlots, static_cast<uint8_t>(1u << top));
            runtime->callGuestFunction(rdram, ctx, s_addresses->guitarWatcherNonStrumSwing, {watcher, top, 0u});
            store<uint8_t>(rdram, gem + kGemSlots, slots);
            s_chordGem = 0u, s_chordFrets = 0u;
        }

        PS2Runtime::RecompiledFunction s_fretButtonUp = nullptr;
        uint32_t s_hammeredOpen = 0u;

        // GuitarTrackWatcherImpl::FretButtonUp(int slot), 0x24d938: an open
        // with every fret let go (those held for sustains aside).
        void fretButtonUp(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t watcher = GPR_U32(ctx, 4);
            callOriginal(rdram, ctx, runtime, s_addresses->guitarWatcherFretButtonUp, s_fretButtonUp);
            if (!s_loading)
                return;
            const int32_t index = closestUnplayed(rdram, ctx, runtime, watcher);
            if (index < 0)
                return;
            const uint32_t gem = watcherGem(rdram, watcher, index);
            const uint8_t slots = load<uint8_t>(rdram, gem + kGemSlots);
            if (load<uint32_t>(rdram, watcher + kWatcherHeld) & ~sustainedLanes(rdram, watcher, slots))
                return;
            if (slots != 0u || !(load<uint8_t>(rdram, gem + kGemFlags) & 1u))
                return;
            store<uint8_t>(rdram, gem + kGemSlots, static_cast<uint8_t>(1u << kOpenLane));
            s_hammeredOpen = gem;
            runtime->callGuestFunction(rdram, ctx, s_addresses->guitarWatcherNonStrumSwing, {watcher, kOpenLane, 0u});
            store<uint8_t>(rdram, gem + kGemSlots, 0u);
            s_hammeredOpen = 0u;
        }

        PS2Runtime::RecompiledFunction s_fretButtonDown = nullptr;

        // GuitarTrackWatcherImpl::FretButtonDown(int slot), 0x24d890: a chord
        // hammered on. (An open's sustain, let go by any fret, is
        // gameplay/extended/guitar/sustain_hold'.)
        void fretButtonDown(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t watcher = GPR_U32(ctx, 4);
            callOriginal(rdram, ctx, runtime, s_addresses->guitarWatcherFretButtonDown, s_fretButtonDown);
            if (!s_loading)
                return;
            if (const int32_t index = closestUnplayed(rdram, ctx, runtime, watcher); index >= 0)
                hammerChord(rdram, ctx, runtime, watcher, index);
        }
        // GuitarTrackWatcherImpl's gem i may be hammered on (0x24df30, from
        // TryToHopo): its HOPO flag, and the gem before it the last hit, so
        // never the first nor one after a miss. A tap may, whatever is
        // before it, as Clone Hero's. A chord never, there: GH2 would try it
        // as the one fret pressed, and
        // a fret not the gem's is a wrong one (TryToHopo, 0x24dd88). Only the
        // chord being hammered on (fretButtonDown), as its highest fret.
        PS2Runtime::RecompiledFunction s_canHopo = nullptr;
        void canHopo(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t watcher = GPR_U32(ctx, 4);
            const auto index = static_cast<int32_t>(GPR_U32(ctx, 5));
            const uint32_t gem = watcherGem(rdram, watcher, index);
            const uint8_t slots = load<uint8_t>(rdram, gem + kGemSlots);
            const auto answer = [&](bool yes)
            {
                SET_GPR_U32(ctx, 2, yes ? 1u : 0u);
                ctx->pc = GPR_U32(ctx, 31);
            };
            if (index >= 0 && gem == s_chordGem)
                return answer(true);
            if (index >= 0 && __builtin_popcount(slots) > 1)
                return answer(false);
            if (index >= 0 && (load<uint8_t>(rdram, gem + kGemFlags) & 1u) && tapGem(rdram, watcher, gem))
                return answer(true);
            s_canHopo(rdram, ctx, runtime);
        }
    }

    uint32_t hammeredOpen()
    {
        return s_hammeredOpen;
    }

    void installHammerOns(PS2Runtime &runtime, const Addresses &addresses)
    {
        s_addresses = &addresses;
        s_canHopo = runtime.lookupFunction(addresses.guitarWatcherCanHopo);
        runtime.replaceFunction(addresses.guitarWatcherCanHopo, &canHopo);
        s_fretButtonUp = runtime.lookupFunction(addresses.guitarWatcherFretButtonUp);
        runtime.replaceFunction(addresses.guitarWatcherFretButtonUp, &fretButtonUp);
        s_fretButtonDown = runtime.lookupFunction(addresses.guitarWatcherFretButtonDown);
        runtime.replaceFunction(addresses.guitarWatcherFretButtonDown, &fretButtonDown);
        s_numSlots = runtime.lookupFunction(addresses.gameGemNumSlots);
        runtime.replaceFunction(addresses.gameGemNumSlots, &scoredSlots<&s_numSlots>);
        s_gemNumSlots = runtime.lookupFunction(addresses.gemNumSlots);
        runtime.replaceFunction(addresses.gemNumSlots, &scoredSlots<&s_gemNumSlots>);
    }
}
