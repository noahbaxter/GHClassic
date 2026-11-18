#include "gameplay/extended/guitar/open_chord_hits.h"

#include "gameplay/extended/guitar/gem_kinds.h"
#include "gameplay/extended/guitar/sustain_hold.h"
#include "guest/beatmatch.h"
#include "hook.h"

#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"

#include <algorithm>
#include <iterator>
#include <utility>

namespace gh2::guitar
{
    namespace
    {
        using guest::GameGem;
        using guest::GuitarTrackWatcherImpl;

        const Addresses *s_addresses = nullptr;

        // Whether the watcher's GameGem is an open chord.
        bool openChord(uint8_t *rdram, uint32_t watcher, uint32_t gem)
        {
            const GameGem view(rdram, gem);
            const Gh2Gem *chart = chartGem(guest::TrackWatcherImpl(rdram, watcher).track(),
                                           static_cast<uint32_t>(view.tick()), view.slots());
            return chart && chart->openChord;
        }

        // The watcher matching frets to a gem, while Swing or
        // TryToFinishSwing runs.
        uint32_t s_matching = 0u;
        template <uint32_t Addresses::*At, PS2Runtime::RecompiledFunction *Original>
        void matching(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t before = std::exchange(s_matching, GPR_U32(ctx, 4));
            callOriginal(rdram, ctx, runtime, s_addresses->*At, *Original);
            s_matching = before;
        }
        PS2Runtime::RecompiledFunction s_swing = nullptr, s_tryToFinishSwing = nullptr;

        // Where those ask GameGem::NumSlots whether a gem is one fret,
        // matched by the highest held, or a chord, by every fret held:
        // Swing's hit and wrong-fret tests, TryToFinishSwing's two. An open
        // chord of one fret is a chord there.
        constexpr uint32_t kMatchedAt[] = {0x24d698u, 0x24d77cu, 0x24db10u, 0x24dbb8u};
        PS2Runtime::RecompiledFunction s_numSlots = nullptr;
        void numSlots(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t from = GPR_U32(ctx, 31), gem = GPR_U32(ctx, 4), watcher = s_matching;
            s_numSlots(rdram, ctx, runtime);
            if (watcher != 0u && GPR_U32(ctx, 2) == 1u &&
                std::find(std::begin(kMatchedAt), std::end(kMatchedAt), from) != std::end(kMatchedAt) &&
                openChord(rdram, watcher, gem))
                SET_GPR_U32(ctx, 2, 2u);
        }

        // GuitarTrackWatcherImpl's gem i may be hammered on (0x24df30, from
        // TryToHopo, which takes one fret by the highest held): an open chord
        // only with exactly its frets held, those held for sustains aside
        // (gameplay/extended/guitar/sustain_hold.h). Else no hammer-on, nor a
        // wrong one's lock.
        PS2Runtime::RecompiledFunction s_canHopo = nullptr;
        void canHopo(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t watcher = GPR_U32(ctx, 4);
            const auto index = static_cast<int32_t>(GPR_U32(ctx, 5));
            const GameGem gem(rdram, index >= 0 ? watcherGem(rdram, watcher, index) : 0u);
            if (index >= 0 && openChord(rdram, watcher, gem.address) &&
                (GuitarTrackWatcherImpl(rdram, watcher).held() & ~sustainedLanes(rdram, watcher, gem.slots())) != gem.slots())
            {
                SET_GPR_U32(ctx, 2, 0u);
                ctx->pc = GPR_U32(ctx, 31);
                return;
            }
            s_canHopo(rdram, ctx, runtime);
        }
    }

    void installOpenChordHits(PS2Runtime &runtime, const Addresses &addresses)
    {
        s_addresses = &addresses;
        replaceKeeping(runtime, addresses.guitarWatcherSwing, &matching<&Addresses::guitarWatcherSwing, &s_swing>,
                       s_swing);
        replaceKeeping(runtime, addresses.guitarWatcherTryToFinishSwing,
                       &matching<&Addresses::guitarWatcherTryToFinishSwing, &s_tryToFinishSwing>, s_tryToFinishSwing);
        replaceKeeping(runtime, addresses.gameGemNumSlots, &numSlots, s_numSlots);
        replaceKeeping(runtime, addresses.guitarWatcherCanHopo, &canHopo, s_canHopo);
    }
}
