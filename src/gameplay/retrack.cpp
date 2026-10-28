// A song's tracks made again mid-song (game_settings.dta's retrack, for note
// speed, highway length and lefty) are made as at its start
// (TrackPanel::Reset, retail 0x153c38) and jumped to where it is:
// GemManager::Jump (0x156da0) shows the gems from the first to start after
// now (upper_bound, 0x156e48), each as never played. So a sustain begun
// before, held, let go or missed, went while its tail still had to run, and
// every hit or miss on screen was forgotten. Here the gems each track shows
// are kept as Reset starts, and the new track's Jump shows from the first of
// them, each put back as it was the first time it is polled.
//
// TrackPanel: its Tracks at +0x60, each one's GemManager at +0x50.
// GemManager: its Gems at +0x04 (0x20 each), the first shown and the one
// past the last at +0x20 and +0x24, the gems hit and held a list at +0x18
// (a node's ms at +0x08, its gem at +0x0c), and whether a star phrase is
// lost at +0x2f8, which Jump clears. Gem: its start and end along the track
// at +0x10 and +0x14, where its tail starts from its start at +0x18, and
// hit, missed and let go in +0x1c.

#include "gameplay/retrack.h"

#include "guest.h"
#include "hook.h"

#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"

#include <deque>
#include <map>
#include <optional>
#include <unordered_map>
#include <vector>

namespace gh2
{
    namespace
    {
        const Addresses *s_addresses = nullptr;

        constexpr uint32_t kPanelTracks = 0x60u, kTrackGems = 0x50u;
        constexpr uint32_t kManagerGems = 0x04u, kManagerBegin = 0x20u, kManagerEnd = 0x24u, kManagerHeld = 0x18u,
                           kManagerPhraseLost = 0x2f8u;
        constexpr uint32_t kHeldMs = 0x08u, kHeldGem = 0x0cu;
        constexpr uint32_t kGemSize = 0x20u, kGemStart = 0x10u, kGemEnd = 0x14u, kGemTail = 0x18u, kGemFlags = 0x1cu;
        constexpr uint32_t kHit = 0x100u, kMissed = 0x200u, kLetGo = 0x400u;
        constexpr uint32_t kFromJump = 0x156e50u; // Jump, upper_bound

        // A gem a track showed, as it was.
        struct Shown
        {
            uint32_t index, flags;
            float start, end, tail;
            bool held;
            float heldMs;
            uint32_t phrase;
        };
        struct Kept
        {
            uint32_t gems, begin, end, lost;
            std::vector<Shown> shown;
        };
        // Each track's, in the order SyncToTime jumps them, and the one
        // being jumped.
        std::deque<Kept> s_kept;
        std::optional<Kept> s_jumping;
        uint32_t s_jumpingManager = 0u;
        // By the new track's Gem, and its manager's phrase lost.
        struct Restore
        {
            uint32_t manager;
            Shown was;
        };
        std::unordered_map<uint32_t, Restore> s_restore;
        std::unordered_map<uint32_t, uint32_t> s_lost;
        // The PhraseState each held gem was hit in, by manager and gem.
        std::map<std::pair<uint32_t, uint32_t>, uint32_t> s_phrases;

        uint32_t gemCount(uint8_t *rdram, uint32_t manager)
        {
            return (load<uint32_t>(rdram, manager + kManagerGems + 4u) - load<uint32_t>(rdram, manager + kManagerGems)) /
                   kGemSize;
        }

        // GemManager::Hit(float ms, int gem, PhraseState).
        struct HitTag;
        void onHit(uint8_t *, R5900Context *ctx, PS2Runtime *)
        {
            s_phrases[{GPR_U32(ctx, 4), GPR_U32(ctx, 5)}] = GPR_U32(ctx, 6);
        }

        // TrackPanel::Reset: its old tracks, still there.
        struct ResetTag;
        void onReset(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            const uint32_t panel = GPR_U32(ctx, 4);
            s_kept.clear(), s_restore.clear(), s_lost.clear(), s_jumping.reset();
            for (uint32_t at = load<uint32_t>(rdram, panel + kPanelTracks); at < load<uint32_t>(rdram, panel + kPanelTracks + 4u);
                 at += 4u)
            {
                const uint32_t manager = load<uint32_t>(rdram, load<uint32_t>(rdram, at) + kTrackGems);
                const uint32_t gems = load<uint32_t>(rdram, manager + kManagerGems);
                Kept kept{gemCount(rdram, manager), load<uint32_t>(rdram, manager + kManagerBegin),
                          load<uint32_t>(rdram, manager + kManagerEnd), load<uint32_t>(rdram, manager + kManagerPhraseLost), {}};
                std::map<uint32_t, float> held;
                const uint32_t list = manager + kManagerHeld;
                for (uint32_t node = load<uint32_t>(rdram, list); node != list; node = load<uint32_t>(rdram, node))
                    held[load<uint32_t>(rdram, node + kHeldGem)] = load<float>(rdram, node + kHeldMs);
                for (uint32_t i = kept.begin; i < kept.end; ++i)
                {
                    const uint32_t gem = gems + i * kGemSize;
                    const auto heldAt = held.find(i);
                    const auto phrase = s_phrases.find({manager, i});
                    kept.shown.push_back({i, load<uint32_t>(rdram, gem + kGemFlags) & (kHit | kMissed | kLetGo),
                                          load<float>(rdram, gem + kGemStart), load<float>(rdram, gem + kGemEnd),
                                          load<float>(rdram, gem + kGemTail), heldAt != held.end(),
                                          heldAt != held.end() ? heldAt->second : 0.0f,
                                          phrase != s_phrases.end() ? phrase->second : 0u});
                }
                s_kept.push_back(std::move(kept));
            }
            s_phrases.clear();
        }

        // GemManager::Jump(float ms): the next track's kept gems, its gems
        // the same.
        struct JumpTag;
        void onJump(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            s_jumping.reset();
            s_jumpingManager = GPR_U32(ctx, 4);
            if (s_kept.empty())
                return;
            if (s_kept.front().gems == gemCount(rdram, s_jumpingManager))
                s_jumping = std::move(s_kept.front());
            s_kept.pop_front();
        }

        // upper_bound(Gem *first, Gem *last, const Gem &): from Jump, the
        // first gem shown; from its kept first if now is among those kept.
        PS2Runtime::RecompiledFunction s_upperBound = nullptr;
        void upperBound(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t from = GPR_U32(ctx, 31), first = GPR_U32(ctx, 4);
            s_upperBound(rdram, ctx, runtime);
            const uint32_t returnTo = ctx->pc;
            if (from == kFromJump && s_jumping)
            {
                const Kept kept = std::move(*s_jumping);
                s_jumping.reset();
                const uint32_t found = (GPR_U32(ctx, 2) - first) / kGemSize;
                if (found > kept.begin && found <= kept.end)
                {
                    for (const Shown &shown : kept.shown)
                        if (shown.flags != 0u || shown.held || shown.index < found)
                            s_restore[first + shown.index * kGemSize] = {s_jumpingManager, shown};
                    s_lost[s_jumpingManager] = kept.lost;
                    SET_GPR_U32(ctx, 2, first + kept.begin * kGemSize);
                }
            }
            ctx->pc = returnTo;
        }

        // GemManager::Poll, which Jump calls once its phrase lost is cleared.
        struct ManagerPollTag;
        void onManagerPoll(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            const uint32_t manager = GPR_U32(ctx, 4);
            if (const auto it = s_lost.find(manager); it != s_lost.end())
            {
                store<uint32_t>(rdram, manager + kManagerPhraseLost, it->second);
                s_lost.erase(it);
            }
        }

        // Gem::Poll(float x5), each shown gem's: one kept, put back. Its
        // tail starts as much further along as the track is longer.
        struct GemPollTag;
        void onGemPoll(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t gem = GPR_U32(ctx, 4);
            const auto it = s_restore.find(gem);
            if (it == s_restore.end())
                return;
            const Restore restore = it->second;
            s_restore.erase(it);
            const Shown &was = restore.was;
            float args[5];
            for (int i = 0; i < 5; ++i)
                args[i] = ctx->f[12 + i];
            const float length = was.end - was.start;
            const float scale = length > 0.0f ? (load<float>(rdram, gem + kGemEnd) - load<float>(rdram, gem + kGemStart)) / length : 1.0f;
            if (was.held)
            {
                ctx->f[12] = was.heldMs;
                runtime->callGuestFunction(rdram, ctx, s_addresses->gemManagerHit, {restore.manager, was.index, was.phrase});
            }
            else if (was.flags & kHit)
                runtime->callGuestFunction(rdram, ctx, s_addresses->gemHit, {gem});
            if (was.flags & kMissed)
                runtime->callGuestFunction(rdram, ctx, s_addresses->gemMiss, {gem});
            if (was.flags & kLetGo)
                runtime->callGuestFunction(rdram, ctx, s_addresses->gemRelease, {gem});
            if (was.held || (was.flags & kLetGo))
            {
                ctx->f[12] = was.tail * scale;
                runtime->callGuestFunction(rdram, ctx, s_addresses->gemSetTailStart, {gem});
            }
            SET_GPR_U32(ctx, 4, gem);
            for (int i = 0; i < 5; ++i)
                ctx->f[12 + i] = args[i];
        }
    }

    void installRetrack(PS2Runtime &runtime, const Addresses &addresses)
    {
        s_addresses = &addresses;
        EntryHook<HitTag>::install(runtime, addresses.gemManagerHit, onHit);
        EntryHook<ResetTag>::install(runtime, addresses.trackPanelReset, onReset);
        EntryHook<JumpTag>::install(runtime, addresses.gemManagerJump, onJump);
        s_upperBound = runtime.lookupFunction(addresses.gemUpperBound);
        runtime.replaceFunction(addresses.gemUpperBound, &upperBound);
        EntryHook<ManagerPollTag>::install(runtime, addresses.gemManagerPoll, onManagerPoll);
        EntryHook<GemPollTag>::install(runtime, addresses.gemPoll, onGemPoll);
    }
}
