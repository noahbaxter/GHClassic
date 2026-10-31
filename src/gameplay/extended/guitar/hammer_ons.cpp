#include "gameplay/extended/guitar/hammer_ons.h"

#include "gameplay/engine/note_engine.h"
#include "gameplay/extended/guitar/gem_kinds.h"
#include "gameplay/extended/guitar/sustain_hold.h"
#include "guest.h"
#include "guest/beatmatch.h"
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
            if (s_chordGem != 0u && (asked == s_chordGem || asked == GameGem(rdram, s_chordGem).slots()))
                SET_GPR_U32(ctx, 2, static_cast<uint32_t>(__builtin_popcount(s_chordFrets)));
            else if (GPR_U32(ctx, 2) == 0u)
                SET_GPR_U32(ctx, 2, 1u);
        }
        PS2Runtime::RecompiledFunction s_numSlots = nullptr, s_gemNumSlots = nullptr;

        // By its own frets, those of the chord or open hammered on as one fret
        // too (gemFrets).
        bool tapGem(uint8_t *rdram, uint32_t watcher, uint32_t gem)
        {
            return isTap(guest::TrackWatcherImpl(rdram, watcher).track(),
                         static_cast<uint32_t>(GameGem(rdram, gem).tick()), gemFrets(rdram, gem));
        }
        // GH2's own rule for a HOPO (0x24df30): the gem before it the last
        // hit.
        bool hopoFollows(uint8_t *rdram, uint32_t watcher, int32_t index)
        {
            const guest::TrackWatcherImpl view(rdram, watcher);
            return index > 0 && view.played(index - 1) && view.lastHit() == index - 1;
        }

        // The gem tried as a hammer-on or pull-off at `fret` alone, shown to
        // GH2 as that fret's for NonStrumSwing (0x24d828), which times and
        // hits it.
        void swingAs(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t watcher, uint32_t gem, uint32_t fret)
        {
            const GameGem view(rdram, gem);
            const uint8_t slots = view.slots();
            view.setSlots(static_cast<uint8_t>(1u << fret));
            runtime->callGuestFunction(rdram, ctx, s_addresses->guitarWatcherNonStrumSwing, {watcher, fret, 0u});
            view.setSlots(slots);
        }

        // A HOPO or tap chord hammered on, or pulled off to, once a fret
        // pressed or let go makes the frets held its own (those held for a
        // sustain aside, gameplay/extended/guitar/sustain_hold.h): shown to GH2
        // as its highest fret alone (the others held below it, which a
        // hammer-on allows), scored as its frets (scoredSlots). A tap's
        // whatever is before it, a HOPO's after the last hit.
        void hammerChord(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t watcher, int32_t index)
        {
            const GameGem gem(rdram, watcherGem(rdram, watcher, index));
            const uint8_t slots = gem.slots();
            const uint32_t held =
                GuitarTrackWatcherImpl(rdram, watcher).held() & ~sustainedLanes(rdram, watcher, slots);
            if (__builtin_popcount(slots) < 2 || !gem.hopo() || held != slots ||
                !(tapGem(rdram, watcher, gem.address) || hopoFollows(rdram, watcher, index)))
                return;
            s_chordGem = gem.address, s_chordFrets = slots;
            swingAs(rdram, ctx, runtime, watcher, gem.address, 31u - static_cast<uint32_t>(__builtin_clz(slots)));
            s_chordGem = 0u, s_chordFrets = 0u;
        }

        PS2Runtime::RecompiledFunction s_fretButtonUp = nullptr;

        // GuitarTrackWatcherImpl::FretButtonUp(int slot), 0x24d938: a chord
        // pulled off to, or an open with every fret let go (those held for
        // sustains aside). GH2 tries a hammer-on or pull-off (TryToHopo,
        // 0x24dc20, by way of NonStrumSwing) for a fret pressed or one left
        // held, never for none, and matches the gem's frets to that one
        // alone: so an open HOPO is tried as the middle lane's.
        void fretButtonUp(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t watcher = GPR_U32(ctx, 4);
            callOriginal(rdram, ctx, runtime, s_addresses->guitarWatcherFretButtonUp, s_fretButtonUp);
            if (!playingCustom())
                return;
            const int32_t index = closestUnplayed(rdram, ctx, runtime, watcher);
            if (index < 0)
                return;
            const GameGem gem(rdram, watcherGem(rdram, watcher, index));
            if (GuitarTrackWatcherImpl(rdram, watcher).held() & ~sustainedLanes(rdram, watcher, gem.slots()))
                return hammerChord(rdram, ctx, runtime, watcher, index);
            if (gem.slots() != 0u || !gem.hopo())
                return;
            s_hammeredOpen = gem.address;
            swingAs(rdram, ctx, runtime, watcher, gem.address, kOpenLane);
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
            if (!playingCustom())
                return;
            if (const int32_t index = closestUnplayed(rdram, ctx, runtime, watcher); index >= 0)
                hammerChord(rdram, ctx, runtime, watcher, index);
        }

        // GuitarTrackWatcherImpl's gem i may be hammered on (0x24df30, from
        // TryToHopo): its HOPO flag, and the gem before it the last hit, so
        // never the first nor one after a miss. A tap may, whatever is
        // before it, as Clone Hero's, a wrong fret pressed at it too:
        // TryToHopo asks here before it looks at its lock. A chord never,
        // there: GH2 would try it as the one fret pressed, and a fret not the
        // gem's is a wrong one (TryToHopo, 0x24dd88). Only the chord being
        // hammered on or pulled off to (hammerChord), as its highest fret.
        // With kTapAntiGhosting a tap keeps the lock a wrong fret pressed in
        // its window puts on it (YARG's default AntiGhosting); without, a
        // ghost never stops a tap, as Clone Hero's.
        constexpr bool kTapAntiGhosting = false;
        PS2Runtime::RecompiledFunction s_canHopo = nullptr;
        void canHopo(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const GuitarTrackWatcherImpl watcher(rdram, GPR_U32(ctx, 4));
            const auto index = static_cast<int32_t>(GPR_U32(ctx, 5));
            const GameGem gem(rdram, watcherGem(rdram, watcher.address, index));
            const auto answer = [&](bool yes)
            {
                SET_GPR_U32(ctx, 2, yes ? 1u : 0u);
                ctx->pc = GPR_U32(ctx, 31);
            };
            if (index >= 0 && gem.address == s_chordGem)
                return answer(true);
            if (index >= 0 && __builtin_popcount(gem.slots()) > 1)
                return answer(false);
            if (index >= 0 && gem.hopo() && tapGem(rdram, watcher.address, gem.address))
            {
                if (!kTapAntiGhosting && watcher.hopoLockedGem() == index)
                    watcher.setHopoLockedGem(-1);
                return answer(true);
            }
            s_canHopo(rdram, ctx, runtime);
        }

        // Hammered on with every fret let go, under GH1's engine: a tap, and
        // an open by the let-go that hits it, which counts as a fret held.
        bool strumless(uint8_t *rdram, uint32_t watcher, int32_t index)
        {
            if (!playingCustom() || index < 0)
                return false;
            const uint32_t gem = watcherGem(rdram, watcher, index);
            return gem == s_hammeredOpen || tapGem(rdram, watcher, gem);
        }

        // TrackWatcherImpl::HitGem(float ms, int gem, bool hammered), 0x2488d0:
        // a gem hammered on that GH1's engine let through without a strum
        // stands in for one, so the let-go rule is met after it.
        struct HitGemTag;
        void onHitGem(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            const uint32_t watcher = GPR_U32(ctx, 4);
            if (GPR_U32(ctx, 6) != 0u && strumless(rdram, watcher, static_cast<int32_t>(GPR_U32(ctx, 5))))
                countAsStrum(watcher);
        }
    }

    uint32_t hammeredOpen()
    {
        return s_hammeredOpen;
    }

    uint8_t gemFrets(uint8_t *rdram, uint32_t gem)
    {
        if (gem != 0u && gem == s_hammeredOpen)
            return 0u;
        return gem != 0u && gem == s_chordGem ? s_chordFrets : GameGem(rdram, gem).slots();
    }

    void installHammerOns(PS2Runtime &runtime, const Addresses &addresses)
    {
        s_addresses = &addresses;
        replaceKeeping(runtime, addresses.guitarWatcherCanHopo, &canHopo, s_canHopo);
        replaceKeeping(runtime, addresses.guitarWatcherFretButtonUp, &fretButtonUp, s_fretButtonUp);
        replaceKeeping(runtime, addresses.guitarWatcherFretButtonDown, &fretButtonDown, s_fretButtonDown);
        EntryHook<HitGemTag>::install(runtime, addresses.trackWatcherHitGem, onHitGem);
        setStrumlessGems(&strumless);
        replaceKeeping(runtime, addresses.gameGemNumSlots, &scoredSlots<&s_numSlots>, s_numSlots);
        replaceKeeping(runtime, addresses.gemNumSlots, &scoredSlots<&s_gemNumSlots>, s_gemNumSlots);
    }
}
