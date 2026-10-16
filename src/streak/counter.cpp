// streak_counter: player 0's note streak on a small odometer under the score
// meter, as GH3 has (ui/dta/streak_counter.dta).
//
// It is the score's own odometer done again. The score's is BandScoreDisplay1,
// a scene of its own (hud/gen/score_display.milo) drawn as a member of the
// score meter's Group. Each HUD scene gets a copy of it, streak_counter, that
// draws streak_counter.milo, made from score_display.milo (counter_scene.cpp).
// The HUD feeds it the score each frame (BandScoreDisplay::SetScore); it
// shows the streak, and slides out from behind the shell when script says so
// and back in when it says not (counter_motion.cpp).

#include "streak/counter.h"

#include "disc/ark.h"
#include "dta.h"
#include "guest.h"
#include "hook.h"
#include "milo/milo.h"
#include "script.h"
#include "streak/counter_motion.h"
#include "streak/counter_scene.h"

#include "ps2_runtime_macros.h"

#include <algorithm>
#include <string>
#include <unordered_map>
#include <vector>

namespace gh2
{
    namespace
    {
        using streak_counter::Counter;

        std::unordered_map<uint32_t, Counter> s_counters;
        const Addresses *s_addresses = nullptr;

        // Where a BandScoreDisplay's Hmx::Object, a virtual base, sits: script
        // hands over that, SetScore takes the display (seen in game, 0x250
        // apart; its player is at +0x248, HudPanel::Poll 0x12bf6c).
        constexpr uint32_t kDisplayObject = 0x250u;
        // Its wheels and Mats (SyncScore, 0x291b20): its display meshes and
        // digit Mats, vectors of 12-byte ObjPtrs, the object at +8.
        constexpr uint32_t kMeshes = 0x214u, kDigitMats = 0x228u;
        // A Group's RndTransformable, from the Hmx::Object script hands over
        // (its local place found at -0xc0 and its world's at -0x80).
        constexpr uint32_t kGroupObject = 0x110u;

        uint32_t displayOf(const script::Call &call)
        {
            const uint32_t object = call.object(1);
            return object > kDisplayObject ? object - kDisplayObject : 0u;
        }

        // {ghc_streak_counter <display> <count> <out>}
        script::Node streakCounter(const script::Call &call)
        {
            if (const uint32_t display = displayOf(call))
            {
                Counter &counter = s_counters[display];
                counter.streak = static_cast<int32_t>(call.number(2));
                counter.shown = call.number(3) != 0.0f;
            }
            return {};
        }

        // {ghc_streak_counter_place <display> <x> <y> <z> <drop> <lift> <view>...}:
        // where it rests, in its parent's space, how far below that it
        // hides, and the views it raises all song, by how much in their
        // parent's.
        script::Node streakCounterPlace(const script::Call &call)
        {
            const uint32_t display = displayOf(call);
            if (!display || call.size() < 7)
                return {};
            Counter &counter = s_counters[display];
            for (int axis = 0; axis < 3; ++axis)
                counter.at[axis] = call.number(2 + axis);
            counter.drop = call.number(5);
            counter.lift = call.number(6);
            counter.placed = true;
            counter.raised.clear();
            for (int i = 7; i < call.size(); ++i)
                if (const uint32_t view = call.object(i); view > kGroupObject)
                    counter.raised.push_back({view - kGroupObject});
            return {};
        }

        // {ghc_streak_counter_mats <display> <score_0.mat> <score_ones_0.mat> ... <score_ones_9.mat> <streak_icon.mat>}:
        // script finds a Mat as its Hmx::Object, the display keeps its
        // RndMat, and the two are as far apart for every Mat.
        script::Node streakCounterMats(const script::Call &call)
        {
            const uint32_t display = displayOf(call), zero = call.object(2);
            if (!display || !zero || call.size() < 14)
                return {};
            const uint32_t digits = load<uint32_t>(call.rdram, display + kDigitMats);
            const uint32_t delta = zero - load<uint32_t>(call.rdram, digits + 8u);
            Counter &counter = s_counters[display];
            for (int n = 0; n <= 9; ++n)
                if (const uint32_t mat = call.object(3 + n))
                    counter.ones[static_cast<size_t>(n)] = mat - delta;
            if (const uint32_t icon = call.object(13))
                counter.icon = icon - delta;
            return {};
        }

        // The HUD gives each score display its player's score a frame: a
        // counter takes its streak instead, and moves.
        struct SetScoreTag;
        void onSetScore(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const auto it = s_counters.find(GPR_U32(ctx, 4));
            if (it == s_counters.end())
                return;
            const EntryArgs args(ctx);
            streak_counter::step(rdram, ctx, runtime, *s_addresses, it->first, it->second);
            args.restore(ctx);
            SET_GPR_U32(ctx, 5, static_cast<uint32_t>(it->second.showing));
        }

        // A counter's wheel (SyncScore sets them from the right, the ones
        // at count - 1, then the blank Mat on the rest): its ones with its
        // own Mats, the blanks after the first zeros, and its first the
        // icon, until a streak of 1000 puts a digit there.
        struct SetDigitMatTag;
        void onSetDigitMat(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            const uint32_t display = GPR_U32(ctx, 4);
            const auto it = s_counters.find(display);
            if (it == s_counters.end() || !it->second.icon)
                return;
            const Counter &counter = it->second;
            const uint32_t wheels = (load<uint32_t>(rdram, display + kMeshes + 4u) - load<uint32_t>(rdram, display + kMeshes)) / 12u;
            const uint32_t which = GPR_U32(ctx, 5), mat = GPR_U32(ctx, 6);
            const uint32_t digits = load<uint32_t>(rdram, display + kDigitMats);
            int digit = -1;
            for (int n = 0; n <= 9; ++n)
                if (load<uint32_t>(rdram, digits + 12u * static_cast<uint32_t>(n) + 8u) == mat)
                    digit = n;
            uint32_t now = mat;
            if (which + 1u == wheels)
                now = counter.ones[static_cast<size_t>(std::max(digit, 0))];
            else if (digit < 0)
                now = which == 0u ? counter.icon : load<uint32_t>(rdram, digits + 8u);
            if (now)
                SET_GPR_U32(ctx, 6, now);
        }

        struct HudEnterTag;
        void onHudEnter(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const EntryArgs args(ctx);
            s_counters.clear();
            script::run(rdram, ctx, runtime, "{streak_counter_enter}");
            args.restore(ctx);
        }
    }

    void installStreakCounter(PS2Runtime &runtime, const Addresses &addresses)
    {
        s_addresses = &addresses;
        script::runWhenUiReady(kStreakCounterDta);
        script::addCommand("ghc_streak_counter", streakCounter);
        script::addCommand("ghc_streak_counter_place", streakCounterPlace);
        script::addCommand("ghc_streak_counter_mats", streakCounterMats);
        EntryHook<HudEnterTag>::install(runtime, addresses.hudPanelEnter, onHudEnter);
        EntryHook<SetScoreTag>::install(runtime, addresses.bandScoreDisplaySetScore, onSetScore);
        EntryHook<SetDigitMatTag>::install(runtime, addresses.bandScoreDisplaySetDigitMat, onSetDigitMat);
        ark::addMade(streak_counter::kScenePath, streak_counter::makeScene);
        for (const std::string &path : ark::paths(0u, "hud/gen/"))
            if (path.ends_with(".milo_ps2"))
                ark::addFix(0u, path, [](std::vector<uint8_t> &file)
                            {
                                auto scene = milo::read(file);
                                if (scene && streak_counter::addCounter(*scene))
                                    file = milo::write(*scene);
                            });
    }
}
