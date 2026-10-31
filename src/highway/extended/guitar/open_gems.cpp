// Gem::AddRep (0x157960) adds one rep for each fret the gem has, so a gem with
// none gets none: here it gets the middle lane's, its parts then each shown
// as the wide mesh of the same name, again each time GemRep::SetType takes
// its template's back (as the gem turns star or is missed). The wide ones
// are highway/extended/guitar/open_pieces'.

#include "highway/extended/guitar/open_gems.h"

#include "gameplay/extended/guitar/gem_kinds.h"
#include "guest.h"
#include "highway/cyan_stars.h"
#include "highway/extended/guitar/open_pieces.h"
#include "highway/extended/guitar/open_tails.h"
#include "highway/extended/guitar/tap_gems.h"
#include "highway/gem_looks.h"
#include "hook.h"
#include "milo/layout.h"
#include "milo/milo.h"

#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <iostream>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace gh2::guitar
{
    namespace
    {
        const Addresses *s_addresses = nullptr;

        // Each open rep's Gem.
        std::map<uint32_t, uint32_t> s_openReps;

        // A star power open's cap: its streak runs right to left, a bar's
        // length in this many seconds, its uv offset in tex_xfm, about the
        // origin, wrapping. Each scene's materials set up once.
        constexpr double kStreakSeconds = 1.6;
        std::set<uint32_t> s_scenes, s_streakMats;

        void setUpStreaks(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t scene)
        {
            if (!s_scenes.insert(scene).second)
                return;
            for (const std::string &name : openStreakMats())
                if (const uint32_t mat = findObject(rdram, ctx, runtime, scene, name))
                {
                    store<uint32_t>(rdram, mat + ::milo::mat::kTexGen, ::milo::mat::kTexGenXfmOrigin);
                    store<uint32_t>(rdram, mat + ::milo::mat::kTexWrap, 1u);
                    s_streakMats.insert(mat);
                }
        }

        // GemRepTemplate::DrawShowing(DrawType): the streaks moved on.
        struct ScrollTag;
        void onScroll(uint8_t *rdram, R5900Context *, PS2Runtime *)
        {
            if (s_streakMats.empty())
                return;
            const double seconds =
                std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
            const float along = static_cast<float>(std::fmod(seconds / kStreakSeconds, 1.0));
            for (const uint32_t mat : s_streakMats)
                store<float>(rdram, mat + ::milo::mat::kTexXfm + 0x30u, along);
        }

        constexpr const char *kStarShine = "star2.mesh";

        // Each of the rep's parts shown as its wide mesh, where the scene has
        // that, and its tail an open's.
        void widenParts(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t rep)
        {
            dressOpenTail(rdram, ctx, runtime, rep);
            const uint32_t type = load<uint32_t>(rdram, rep + gemRep::kType);
            const auto open = s_openReps.find(rep);
            const bool phrase = open != s_openReps.end() && inStarPhrase(open->second);
            const bool tap = open != s_openReps.end() && isTapGem(open->second);
            for (const uint32_t part : gemRep::kParts)
            {
                const uint32_t object = partObject(rdram, rep, part);
                const std::string name(objectName(rdram, object));
                if (object == 0u || name.rfind("open_", 0u) == 0u)
                    continue;
                // A star's shine off for an open's. Shown in a template of
                // its own, every star's went with it.
                if (name == kStarShine)
                {
                    runtime->callGuestFunction(rdram, ctx, s_addresses->gemRepPartSetShowing, {rep + part, 0u});
                    continue;
                }
                const uint32_t scene = load<uint32_t>(rdram, object + ::milo::object::kDir);
                const uint32_t found = findObject(rdram, ctx, runtime, scene, openName(name, type, phrase, tap));
                if (found == 0u)
                    continue;
                setUpStreaks(rdram, ctx, runtime, scene);
                showPartAs(rdram, ctx, runtime, rep, part, found);
            }
        }

        PS2Runtime::RecompiledFunction s_addRep = nullptr, s_setType = nullptr, s_returnGem = nullptr;

        // A pool's reps are any lane's: GemRep::Init sets only what a lane's
        // differ in, so an open's, its every part changed, are kept apart,
        // each pool's free ones here and those out by the pool they go back
        // to.
        std::map<uint32_t, std::vector<uint32_t>> s_openFree;
        std::map<uint32_t, uint32_t> s_openOut;

        // An open's free rep, or a new one, last on the pool's free list,
        // which GemRepPool::GetGem takes from.
        void offerOpenRep(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t pool)
        {
            auto &free = s_openFree[pool];
            uint32_t rep = 0u;
            if (free.empty())
                rep = static_cast<uint32_t>(
                    runtime->callGuestFunction(rdram, ctx, s_addresses->gemRepPoolCreateGem, {pool}));
            else
                rep = free.back(), free.pop_back();
            runtime->callGuestFunction(rdram, ctx, s_addresses->gemRepPoolReturnGem, {pool, rep});
        }

        // GemRepPool::ReturnGem(GemRep *): an open's to the opens'.
        void returnGem(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t pool = GPR_U32(ctx, 4), rep = GPR_U32(ctx, 5);
            const auto out = s_openOut.find(rep);
            if (out == s_openOut.end())
                return s_returnGem(rdram, ctx, runtime);
            s_openFree[pool].push_back(rep);
            s_openOut.erase(out);
            ctx->pc = GPR_U32(ctx, 31);
        }

        // ~GemRepPool: the opens' free reps on its free list, deleted with
        // it, and the track's scene going with it.
        void trackGone(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t pool)
        {
            for (const uint32_t rep : s_openFree[pool])
                runtime->callGuestFunction(rdram, ctx, s_addresses->gemRepPoolReturnGem, {pool, rep});
            s_openFree.erase(pool);
            // Those still out are deleted with their gems (~Gem), never
            // through Gem::RemoveRep.
            for (auto out = s_openOut.begin(); out != s_openOut.end();)
                if (out->second == pool)
                {
                    s_openReps.erase(out->first);
                    out = s_openOut.erase(out);
                }
                else
                    ++out;
            s_scenes.clear(), s_streakMats.clear();
        }

        // Gem::AddRep(GemRepPool &, RndGroup *, GemRep::Type, const
        // TrackConfig &), the config in $t0, which the calls into the guest
        // before AddRepForSlot's may clobber.
        void addRep(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t gem = GPR_U32(ctx, 4);
            if (load<uint8_t>(rdram, gem + gem::kFrets) != 0u)
                return s_addRep(rdram, ctx, runtime);
            const uint32_t returnTo = GPR_U32(ctx, 31), config = GPR_U32(ctx, 8);
            const EntryArgs args(ctx);
            offerOpenRep(rdram, ctx, runtime, args.a[1]);
            runtime->callGuestFunction(rdram, ctx, s_addresses->gemAddRepForSlot,
                                       {gem, args.a[1], args.a[2], kOpenLane, args.a[3], config});
            const uint32_t rep = load<uint32_t>(rdram, load<uint32_t>(rdram, gem + gem::kRepsEnd) - 4u);
            s_openOut[rep] = args.a[1];
            s_openReps[rep] = gem;
            widenParts(rdram, ctx, runtime, rep);
            ctx->pc = returnTo;
        }

        // GemRep::SetType(GemRep::Type), which takes its template's parts.
        void setType(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t rep = GPR_U32(ctx, 4);
            callOriginal(rdram, ctx, runtime, s_addresses->gemRepSetType, s_setType);
            if (s_openReps.count(rep))
                widenParts(rdram, ctx, runtime, rep);
        }

        // Gem::RemoveRep(GemRepPool &): its reps go back to the pool, for any
        // gem's.
        struct RemoveRepTag;
        void onRemoveRep(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            const uint32_t gem = GPR_U32(ctx, 4);
            for (uint32_t r = load<uint32_t>(rdram, gem + gem::kReps), end = load<uint32_t>(rdram, gem + gem::kRepsEnd);
                 r < end; r += 4u)
                s_openReps.erase(load<uint32_t>(rdram, r));
        }
    }

    bool isOpenRep(uint32_t rep)
    {
        return s_openReps.count(rep) != 0u;
    }

    uint32_t openScene()
    {
        return s_scenes.empty() ? 0u : *s_scenes.begin();
    }

    void installOpenGems(PS2Runtime &runtime, const Addresses &addresses)
    {
        s_addresses = &addresses;
        addToTrackScene(addOpenPieces);
        replaceKeeping(runtime, addresses.gemAddRep, &addRep, s_addRep);
        replaceKeeping(runtime, addresses.gemRepSetType, &setType, s_setType);
        EntryHook<RemoveRepTag>::install(runtime, addresses.gemRemoveRep, onRemoveRep);
        replaceKeeping(runtime, addresses.gemRepPoolReturnGem, &returnGem, s_returnGem);
        onTrackGone(trackGone);
        EntryHook<ScrollTag>::install(runtime, addresses.gemRepTemplateDrawShowing, onScroll);
    }
}
