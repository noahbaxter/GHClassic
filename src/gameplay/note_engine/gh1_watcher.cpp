// GH1's guitar watcher (TrackBeatMatcher, GH1 debug 0x2805f0): GH2's with
// these differences, each put back here.
//
// - A gem is a HOPO when it is one fret, not the same frets as the gem
//   before, and at most 159 ticks after it. GH2's must share no fret with
//   the gem before and be under the song's hopo_threshold, 170 unless the
//   song sets one (GameGemList::WillBeNoStrum, GH1 0x277e58).
// - A gem under 121 ticks long has no sustain. GH2's parser cuts at 160
//   and under (GH1 0x27f2a8, GH2 0x245ff8).
// - Once every fret is let go, no hammer-on or pull-off counts until the
//   next strum. A strum counts here even if BeatMatcher::Swing drops it
//   inside its 52 ms limit. GH2 has no such lock (GuitarController::OnMsg,
//   GH1 0x278120).
// - A hammer-on or pull-off may come at most 40 ms early (slop - 60) and
//   aims at the nearest gem, played or not. GH2's may come 100 ms early
//   (slop) and aims at the nearest unplayed gem. Both allow 160 ms late
//   (slop + 60). GH1's may land in the same frame as a hit, GH2's may not
//   (TryToHopo, GH1 0x281650).
// - A gem may be hammered once the gem before it is played. GH2 also wants
//   that gem to be the last one hit, which after a practice jump none is.
// - A wrong pull-off locks the gem, so it must be strummed. A wrong
//   hammer-on does that in both games.
// - A strum whose nearest gem is already played is an overstrum, unless
//   that gem was just hammered and is still within slop. GH2 aims the
//   strum at the gem after instead (Swing, GH1 0x280b38).
// - Every miss counts: GH2 drops one within 50 ms of the last. A miss on a
//   gem already hit or passed goes to the gem after it. GH2 does that only
//   for a passed gem (SendMiss, GH1 0x282160).
// - A HOPO-able gem is passed at slop, where GH2 waits 60 ms more. A gem
//   waiting on a strum's late frets is passed on time like any other, then
//   hit if the frets still come in time. GH2 holds the pass until the
//   strum resolves (CheckForPasses, GH1 0x2818e8, GH2 GemCanBePassed).
// - A strum waiting on its frets misses when its 60 ms run out, before the
//   poll passes gems, and after a hammered gem too. GH2 lets that one go
//   (TrackBeatMatcher::Poll, GH1 0x280a10).
//
// Kept GH2's: the song's sync offset in every window.

#include "gameplay/note_engine/gh1.h"

#include "guest/beatmatch.h"
#include "hook.h"

#include "ps2_runtime_macros.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <unordered_map>

namespace gh2::gh1_rules
{
    namespace
    {
        using guest::BeatMatcher;
        using guest::GameGem;
        using guest::GameGemList;
        using guest::GuitarTrackWatcherImpl;
        using guest::MultiGemInfo;
        using guest::TrackWatcherImpl;

        constexpr int32_t kGh1HopoTicks = 159;
        constexpr int32_t kGh1SustainTicks = 121;

        const Addresses *s_addresses = nullptr;
        // Each guitar watcher's: a strum since every fret was last let go.
        std::unordered_map<uint32_t, bool> s_strummed;

        float nowOf(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, const TrackWatcherImpl &watcher)
        {
            const guest::Object::Virtual getNow = watcher.parent().getNow();
            runtime->callGuestFunction(rdram, ctx, getNow.function, {getNow.self});
            return ctx->f[0];
        }

        // Asked of each gem as the chart is read: GH1's HOPO rule, and its
        // sustain cutoff alongside.
        struct WillBeNoStrumTag;
        bool onWillBeNoStrum(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            const GameGemList list(rdram, GPR_U32(ctx, 4));
            const MultiGemInfo info(rdram, GPR_U32(ctx, 5));
            info.setNoSustain(info.ticks() < kGh1SustainTicks);
            bool hopo = false;
            if (list.size() > 0)
            {
                const GameGem last = list[list.size() - 1];
                hopo = info.slots() != last.slots() && std::popcount(info.slots()) == 1 &&
                       info.tick() - last.tick() <= kGh1HopoTicks;
            }
            SET_GPR_U32(ctx, 2, hopo ? 1u : 0u);
            leave(ctx);
            return true;
        }

        // s_strummed, for the lock once every fret is let go: cleared with a
        // new watcher or the last fret up, set by a strum. onTryToHopo reads it.
        struct CtorTag;
        void onCtor(uint8_t *, R5900Context *ctx, PS2Runtime *)
        {
            s_strummed[GPR_U32(ctx, 4)] = false;
        }

        // GH1's controller marks a strum before BeatMatcher::Swing's 52 ms
        // can drop it.
        struct BeatMatcherSwingTag;
        void onBeatMatcherSwing(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            if (GPR_U32(ctx, 6) != 0u)
                s_strummed[BeatMatcher(rdram, GPR_U32(ctx, 4)).swingWatcher()] = true;
        }

        struct FretButtonUpTag;
        void onFretButtonUp(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            const GuitarTrackWatcherImpl watcher(rdram, GPR_U32(ctx, 4));
            if ((watcher.held() & ~(1u << GPR_U32(ctx, 5))) == 0u)
                s_strummed[watcher.address] = false;
        }

        // A hammer-on or pull-off: refused under the let-go lock, on a played
        // gem or too early, else let through as a press so a wrong one locks
        // the gem.
        struct TryToHopoTag;
        bool onTryToHopo(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            const GuitarTrackWatcherImpl watcher(rdram, GPR_U32(ctx, 4));
            const GameGemList gems = watcher.gems();
            if (gems.size() == 0)
                return false;
            const float at = ctx->f[12] + watcher.sync();
            const int c = gems.closest(at);
            const float early = watcher.leniency() - watcher.slop();
            if (!s_strummed[watcher.address] || watcher.done(c) || at - gems[c].ms() < early)
            {
                leave(ctx);
                return true;
            }
            watcher.setLastHitMs(-1000.0f); // read nowhere else
            SET_GPR_U32(ctx, 6, 1u); // isPress, so a wrong pull-off locks the gem too
            return false;
        }

        // Hammerable once the gem before is played, hit last or not.
        struct CanHopoTag;
        bool onCanHopo(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            const TrackWatcherImpl watcher(rdram, GPR_U32(ctx, 4));
            const int g = static_cast<int32_t>(GPR_U32(ctx, 5));
            const bool can = watcher.gems()[g].hopo() && g > 0 && watcher.played(g - 1);
            SET_GPR_U32(ctx, 2, can ? 1u : 0u);
            leave(ctx);
            return true;
        }

        // A strum at a played gem is an overstrum, unless it is the gem just
        // hammered and still within slop.
        struct SwingTag;
        bool onSwing(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const EntryArgs args(ctx);
            const GuitarTrackWatcherImpl watcher(rdram, args.a[0]);
            const GameGemList gems = watcher.gems();
            if (gems.size() == 0)
                return false;
            const float now = nowOf(rdram, ctx, runtime, watcher);
            const float at = now + watcher.sync();
            const int c = gems.closest(at);
            const bool inSlop = std::fabs(at - gems[c].ms()) <= watcher.slop();
            if (watcher.done(c) && (c != watcher.hopoGem() || !inSlop) && c >= watcher.captureEnd())
            {
                ctx->f[12] = now;
                runtime->callGuestFunction(rdram, ctx, s_addresses->trackWatcherOnMiss,
                                           {watcher.address, args.a[1], static_cast<uint32_t>(c)});
                leave(ctx);
                return true;
            }
            args.restore(ctx);
            return false;
        }

        // Every miss counts, and one on a gem already hit or passed goes to
        // the gem after.
        struct SendMissTag;
        void onSendMiss(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            const TrackWatcherImpl watcher(rdram, GPR_U32(ctx, 4));
            watcher.setLastMissMs(-10000.0f);
            const int32_t gem = static_cast<int32_t>(GPR_U32(ctx, 6));
            if (gem != -1 && gem == std::max(watcher.lastHit(), watcher.lastPassed()) && gem + 1 < watcher.gems().size())
                SET_GPR_U32(ctx, 6, static_cast<uint32_t>(gem + 1));
        }

        // Slop is read only for passes: no 60 ms more for a HOPO-able gem.
        struct SlopTag;
        bool onSlop(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            const TrackWatcherImpl watcher(rdram, GPR_U32(ctx, 4));
            ctx->f[0] = watcher.slop() - watcher.sync();
            leave(ctx);
            return true;
        }

        // A gem waiting on a strum's frets is passed on time.
        struct GemCanBePassedTag;
        bool onGemCanBePassed(uint8_t *, R5900Context *ctx, PS2Runtime *)
        {
            SET_GPR_U32(ctx, 2, 1u);
            leave(ctx);
            return true;
        }

        // GH1 times a strum out before it passes gems.
        struct PollTag;
        void onPoll(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const TrackWatcherImpl watcher(rdram, GPR_U32(ctx, 4));
            if (watcher.vtable() != s_addresses->guitarWatcherVtable)
                return;
            const EntryArgs args(ctx);
            runtime->callGuestFunction(rdram, ctx, s_addresses->guitarWatcherCheckForFretTimeout, {watcher.address});
            args.restore(ctx);
        }

        // And misses a timed-out strum after a hammered gem too.
        struct FretTimeoutTag;
        void onCheckForFretTimeout(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            GuitarTrackWatcherImpl(rdram, GPR_U32(ctx, 4)).setPendingAfterHopo(0);
        }
    }

    void installWatcher(PS2Runtime &runtime, const Addresses &addresses)
    {
        s_addresses = &addresses;
        // Under either engine, so GH1's know of every strum and fret.
        EntryHook<CtorTag>::install(runtime, addresses.guitarWatcherCtor, onCtor);
        EntryHook<BeatMatcherSwingTag>::install(runtime, addresses.beatMatcherSwing, onBeatMatcherSwing);
        EntryHook<FretButtonUpTag>::install(runtime, addresses.guitarWatcherFretButtonUp, onFretButtonUp);
        Override<WillBeNoStrumTag>::install(runtime, addresses.willBeNoStrum, gh1Only<onWillBeNoStrum>);
        Override<TryToHopoTag>::install(runtime, addresses.guitarWatcherTryToHopo, gh1Only<onTryToHopo>);
        Override<CanHopoTag>::install(runtime, addresses.guitarWatcherCanHopo, gh1Only<onCanHopo>);
        Override<SwingTag>::install(runtime, addresses.guitarWatcherSwing, gh1Only<onSwing>);
        EntryHook<SendMissTag>::install(runtime, addresses.trackWatcherSendMiss, gh1Only<onSendMiss>);
        Override<SlopTag>::install(runtime, addresses.guitarWatcherSlop, gh1Only<onSlop>);
        Override<GemCanBePassedTag>::install(runtime, addresses.guitarWatcherGemCanBePassed, gh1Only<onGemCanBePassed>);
        EntryHook<PollTag>::install(runtime, addresses.trackWatcherPoll, gh1Only<onPoll>);
        EntryHook<FretTimeoutTag>::install(runtime, addresses.guitarWatcherCheckForFretTimeout, gh1Only<onCheckForFretTimeout>);
    }
}
