#include "gameplay/extended/guitar/lane_hits.h"

#include "custom/gh2_chart.h"
#include "gameplay/extended/guitar/gem_kinds.h"
#include "guest/beatmatch.h"
#include "hook.h"

#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"

#include <algorithm>
#include <bit>
#include <deque>
#include <map>
#include <optional>
#include <vector>

namespace gh2::guitar
{
    namespace
    {
        using guest::GuitarTrackWatcherImpl;

        const Addresses *s_addresses = nullptr;

        // YARG's five-fret LaneAutohitWindow and LaneProximityProtectionWindow
        // (EnginePreset.Instruments), in ms.
        constexpr float kAutohit = 160.0f, kNear = 80.0f;

        // Each watcher's inputs, the last few: when (its gems' ms) and the
        // frets held then.
        struct Input
        {
            float at = 0.0f;
            uint8_t held = 0u;
        };
        constexpr size_t kKept = 64u;
        std::map<uint32_t, std::deque<Input>> s_inputs;

        bool judging() { return playingCustom(); }

        // A lane mask's mark past the frets: an open chord's.
        constexpr uint8_t kOpenChord = 0x20u;

        // The lane gem i is in: its first and last gems, and its masks (a
        // tremolo's one, a trill's two). A Face-Off lane is the player's own
        // gems in it: the other's (the watcher holds every gem, Playable
        // skipping theirs, 0x249088) end it.
        struct Lane
        {
            int first = 0, last = 0;
            std::vector<uint8_t> masks;
            bool trill() const { return masks.size() == 2u; }
        };
        bool laned(uint8_t *rdram, uint32_t watcher, int i, bool *start = nullptr)
        {
            const GuitarTrackWatcherImpl view(rdram, watcher);
            if (i < 0 || i >= view.gems().size() || !view.mine(i))
                return false;
            const Gh2Gem *gem = watcherChartGem(rdram, watcher, i);
            if (start)
                *start = gem && gem->lanePhraseStart;
            return gem && gem->inLanePhrase;
        }
        std::optional<Lane> laneOf(uint8_t *rdram, uint32_t watcher, int i)
        {
            bool start = false;
            if (!laned(rdram, watcher, i, &start))
                return std::nullopt;
            Lane lane{i, i, {}};
            while (!start && laned(rdram, watcher, lane.first - 1, &start))
                --lane.first;
            bool next = false;
            while (laned(rdram, watcher, lane.last + 1, &next) && !next)
                ++lane.last;
            const GuitarTrackWatcherImpl view(rdram, watcher);
            for (int g = lane.first; g <= lane.last && lane.masks.size() < 2u; ++g)
            {
                uint8_t mask = view.gems()[g].slots();
                if (const Gh2Gem *chart = watcherChartGem(rdram, watcher, g); chart && chart->openChord)
                    mask |= kOpenChord;
                if (std::find(lane.masks.begin(), lane.masks.end(), mask) == lane.masks.end())
                    lane.masks.push_back(mask);
            }
            return lane;
        }

        // Whether `held` plays `mask` (GuitarEngine.ActiveLaneIncludesNote): a
        // chord's frets exactly, an open chord's too (kOpenChord marked,
        // gameplay/extended/guitar/open_chord_hits.h), a single note's (or an
        // open's, none) as the highest held.
        bool plays(uint8_t held, uint8_t mask)
        {
            if (mask & kOpenChord)
                return held == (mask & ~kOpenChord);
            if (std::popcount(mask) > 1)
                return held == mask;
            return held == 0u ? mask == 0u : mask != 0u && std::bit_width(held) == std::bit_width(mask);
        }

        // When the player last played `lane` up to `now`: inputs from its
        // first note on, a trill's each its other note from the one before,
        // the first note's other first (SubmitLaneNote's RequiredLaneNote
        // and NextTrillNote). The input hitting a trill's first note counts
        // too, as YARG's hit on a lane's first note starts its window.
        std::optional<float> lastPlayed(uint8_t *rdram, uint32_t watcher, const Lane &lane, float now)
        {
            const GuitarTrackWatcherImpl view(rdram, watcher);
            const float from = view.gems()[lane.first].ms() - view.slop();
            int previous = 0;
            bool hit = lane.trill() && view.played(lane.first);
            std::optional<float> last;
            for (const Input &in : s_inputs[watcher])
            {
                if (in.at < from || in.at > now)
                    continue;
                if (hit && plays(in.held, lane.masks[0]))
                {
                    last = in.at;
                    hit = false;
                    continue;
                }
                for (int m = 0; m < int(lane.masks.size()); ++m)
                    if (plays(in.held, lane.masks[size_t(m)]) && (!lane.trill() || m != previous))
                    {
                        last = in.at;
                        if (lane.trill())
                            previous = m;
                        break;
                    }
            }
            return last;
        }

        void keep(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t watcher, uint8_t held)
        {
            const float at = watcherNow(rdram, ctx, runtime, watcher) + GuitarTrackWatcherImpl(rdram, watcher).sync();
            auto &inputs = s_inputs[watcher];
            // A song begun again starts its clock over.
            while (!inputs.empty() && inputs.back().at > at)
                inputs.pop_back();
            inputs.push_back({at, held});
            if (inputs.size() > kKept)
                inputs.pop_front();
        }

        // GuitarTrackWatcherImpl::Swing(int slot, bool): a strum, kept with
        // the frets held.
        struct SwingTag;
        void onSwing(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (!judging())
                return;
            const EntryArgs args(ctx);
            const uint32_t watcher = args.a[0];
            keep(rdram, ctx, runtime, watcher, static_cast<uint8_t>(GuitarTrackWatcherImpl(rdram, watcher).held()));
            args.restore(ctx);
        }

        // GuitarTrackWatcherImpl::FretButtonDown and FretButtonUp(int slot):
        // a fret pressed or let go (a pull-off's), kept with what is held
        // after.
        template <bool Down>
        void onFret(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (!judging())
                return;
            const EntryArgs args(ctx);
            const uint32_t watcher = args.a[0];
            const uint32_t bit = args.a[1] < 5u ? 1u << args.a[1] : 0u;
            const uint32_t held = GuitarTrackWatcherImpl(rdram, watcher).held();
            keep(rdram, ctx, runtime, watcher, static_cast<uint8_t>(Down ? held | bit : held & ~bit));
            args.restore(ctx);
        }
        struct FretDownTag;
        struct FretUpTag;

        // TrackWatcherImpl::OnMiss(float ms, int slot, int gem), a strum
        // or fret that hits nothing: none while it plays the lane about it,
        // nor near a lane's ends (GuitarEngine's IsInLaneLeniencyWindow).
        struct MissTag;
        bool onMiss(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            if (!judging())
                return false;
            const uint32_t watcher = GPR_U32(ctx, 4);
            const GuitarTrackWatcherImpl view(rdram, watcher);
            const float at = ctx->f[12];
            const uint8_t held = static_cast<uint8_t>(view.held());
            const int count = view.gems().size();
            const int near = view.gems().closest(at);
            for (const int i : {near - 1, near, near + 1})
            {
                const auto lane = i >= 0 && i < count ? laneOf(rdram, watcher, i) : std::nullopt;
                if (!lane)
                    continue;
                const float start = view.gems()[lane->first].ms(), end = view.gems()[lane->last].ms();
                if (at < start - kNear || at > end + kNear)
                    continue;
                bool fits = at < start || at > end;
                for (const uint8_t mask : lane->masks)
                    fits |= plays(held, mask);
                if (fits)
                {
                    leave(ctx);
                    return true;
                }
            }
            return false;
        }

        // TrackWatcherImpl::OnPass(float ms, int gem): a lane's note, not
        // its first, played within kAutohit before it is hit, on time, as
        // autoplay hits (CheckForAutoplay, 0x248c80), for its score, streak
        // and star power.
        struct PassTag;
        bool onPass(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (!judging())
                return false;
            const uint32_t watcher = GPR_U32(ctx, 4);
            const int gem = static_cast<int>(GPR_U32(ctx, 5));
            const auto lane = laneOf(rdram, watcher, gem);
            if (!lane || gem == lane->first)
                return false;
            const float ms = GuitarTrackWatcherImpl(rdram, watcher).gems()[gem].ms();
            const auto played = lastPlayed(rdram, watcher, *lane, ctx->f[12]);
            if (!played || ms > *played + kAutohit)
                return false;
            ctx->f[12] = ms;
            runtime->callGuestFunction(rdram, ctx, s_addresses->trackWatcherHitGem, {watcher, static_cast<uint32_t>(gem), 1u});
            leave(ctx);
            return true;
        }
    }

    void installLaneHits(PS2Runtime &runtime, const Addresses &addresses)
    {
        s_addresses = &addresses;
        EntryHook<SwingTag>::install(runtime, addresses.guitarWatcherSwing, onSwing);
        EntryHook<FretDownTag>::install(runtime, addresses.guitarWatcherFretButtonDown, onFret<true>);
        EntryHook<FretUpTag>::install(runtime, addresses.guitarWatcherFretButtonUp, onFret<false>);
        Override<MissTag>::install(runtime, addresses.trackWatcherOnMiss, onMiss);
        Override<PassTag>::install(runtime, addresses.trackWatcherOnPass, onPass);
    }
}
