#include "gameplay/extended/guitar/sustain_hold.h"

#include "gameplay/extended/guitar/gem_kinds.h"
#include "gameplay/extended/guitar/hammer_ons.h"
#include "guest.h"
#include "guest/beatmatch.h"
#include "hook.h"
#include "settings/settings.h"

#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <vector>

namespace gh2::guitar
{
    namespace
    {
        using guest::GameGem;
        using guest::GuitarTrackWatcherImpl;
        using guest::TrackWatcherImpl;

        const Addresses *s_addresses = nullptr;

        // Player: its track, and the HeldNote it scores (its GameGem, 0 for
        // none, the gem's index, the points it has given and its all).
        constexpr uint32_t kPlayerTrack = 0xa0u, kPlayerHeld = 0xccu;
        using HeldNote = std::array<uint32_t, 4>;

        // A sustain held: its gem and frets, when it was hit and ends (ms),
        // and its HeldNote, taken from its Player.
        struct Held
        {
            int32_t gem;
            uint8_t lanes;
            float hit, end;
            HeldNote note;
        };
        std::map<uint32_t, std::vector<Held>> s_held; // by watcher
        std::map<uint32_t, uint32_t> s_players;       // by track
        std::map<uint32_t, uint32_t> s_watchers;      // by track
        uint32_t s_hitTrack = 0u;                     // the track of the gem HitGem hits

        HeldNote heldNote(uint8_t *rdram, uint32_t player)
        {
            HeldNote note;
            for (uint32_t i = 0; i < note.size(); ++i)
                note[i] = load<uint32_t>(rdram, player + kPlayerHeld + 4u * i);
            return note;
        }
        void setHeldNote(uint8_t *rdram, uint32_t player, const HeldNote &note)
        {
            for (uint32_t i = 0; i < note.size(); ++i)
                store<uint32_t>(rdram, player + kPlayerHeld + 4u * i, note[i]);
        }

        uint32_t playerOf(uint8_t *rdram, uint32_t watcher)
        {
            const auto found = s_players.find(TrackWatcherImpl(rdram, watcher).track());
            return found != s_players.end() ? found->second : 0u;
        }

        // A held sustain's frets whose own sustains are done by `now`, a
        // split chord's (custom/gh2_chart.h), one let go this close to its
        // end (sustain_release_ms) done too.
        uint8_t done(uint8_t *rdram, uint32_t watcher, const Held &held, float now)
        {
            const Gh2Gem *chart = watcherChartGem(rdram, watcher, held.gem);
            if (!chart || !chart->split())
                return 0u;
            const float from = held.hit - static_cast<float>(settings::get(settings::kSustainReleaseMs));
            uint8_t lanes = 0u;
            for (uint32_t lane = 0; lane < 5u; ++lane)
                if (((held.lanes >> lane) & 1u) && from + chart->laneMs[lane] <= now)
                    lanes |= static_cast<uint8_t>(1u << lane);
            return lanes;
        }

        // `call` with `note` as its Player's HeldNote, then taken back as GH2
        // left it, the Player's own put back.
        template <typename Call>
        void asHeldNote(uint8_t *rdram, uint32_t player, HeldNote &note, Call call)
        {
            const HeldNote own = player ? heldNote(rdram, player) : HeldNote{};
            if (player)
                setHeldNote(rdram, player, note);
            call();
            if (player)
            {
                note = heldNote(rdram, player);
                setHeldNote(rdram, player, own);
            }
        }

        // Its Player's HeldNote, as GH2 scores it, at `now`.
        void score(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t player, Held &held, float now)
        {
            if (!player || held.note[0] == 0u)
                return;
            asHeldNote(rdram, player, held.note,
                       [&]
                       {
                           ctx->f[12] = now;
                           runtime->callGuestFunction(rdram, ctx, s_addresses->playerCheckHeldNote, {player});
                       });
        }

        // A sustain let go before its end, as GH2 lets go of one (KillSustain,
        // 0x248ff8, by SendReleaseGem): its HeldNote finished in its Player's
        // place, the track's sound cut, its tail let go.
        void letGo(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t watcher, Held held, float now)
        {
            asHeldNote(rdram, playerOf(rdram, watcher), held.note,
                       [&]
                       {
                           ctx->f[12] = now;
                           ctx->f[13] = held.end - now;
                           runtime->callGuestFunction(rdram, ctx, s_addresses->trackWatcherSendReleaseGem,
                                                      {watcher, static_cast<uint32_t>(held.gem)});
                       });
        }

        // Player::Hit(float, int gem, Scoring::HitType): its track's Player.
        struct PlayerHitTag;
        void onPlayerHit(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            const uint32_t player = GPR_U32(ctx, 4);
            s_players[load<uint32_t>(rdram, player + kPlayerTrack)] = player;
        }

        // TrackWatcherImpl::HitGem(float, int gem, bool): the sustain GH2
        // holds for the gem hit (+0x20, and its Player's HeldNote) taken into
        // the watcher's held ones, GH2 left holding none.
        PS2Runtime::RecompiledFunction s_hitGem = nullptr;
        void hitGem(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const TrackWatcherImpl watcher(rdram, GPR_U32(ctx, 4));
            if (!playingCustom())
                return s_hitGem(rdram, ctx, runtime);
            s_hitTrack = watcher.track();
            s_watchers[s_hitTrack] = watcher.address;
            callOriginal(rdram, ctx, runtime, s_addresses->trackWatcherHitGem, s_hitGem);
            const int32_t gem = watcher.sustained();
            if (gem < 0)
                return;
            const uint32_t game = watcherGem(rdram, watcher.address, gem);
            if (!GameGem(rdram, game).noSustain())
            {
                Held held{gem, gemFrets(rdram, game), watcher.sustainHit(), watcher.sustainEnd(), {}};
                if (const uint32_t player = playerOf(rdram, watcher.address))
                    if (const HeldNote note = heldNote(rdram, player);
                        note[0] != 0u && static_cast<int32_t>(note[1]) == gem)
                    {
                        held.note = note;
                        setHeldNote(rdram, player, {});
                    }
                s_held[watcher.address].push_back(held);
            }
            watcher.setSustained(-1);
            watcher.setSustainEnd(0.0f);
        }

        // Player::Penalize(float ms, int gem), 0x1114c0: a gem missed or
        // passed, or a strum at none. Both engines finish the Player's
        // HeldNote there (FinishHeldNote, 0x112348), so every sustain held
        // is let go too.
        struct PenalizeTag;
        void onPenalize(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (!playingCustom())
                return;
            const auto watcher = s_watchers.find(load<uint32_t>(rdram, GPR_U32(ctx, 4) + kPlayerTrack));
            if (watcher == s_watchers.end())
                return;
            const auto found = s_held.find(watcher->second);
            if (found == s_held.end() || found->second.empty())
                return;
            const EntryArgs args(ctx);
            const float now = watcherNow(rdram, ctx, runtime, watcher->second);
            for (const Held &held : found->second)
                if (held.end > now)
                    letGo(rdram, ctx, runtime, watcher->second, held, now);
            found->second.clear();
            args.restore(ctx);
        }

        // TrackWatcherImpl::Jump(float), to another place in the song.
        struct JumpTag;
        void onJump(uint8_t *, R5900Context *ctx, PS2Runtime *)
        {
            s_held.erase(GPR_U32(ctx, 4));
        }

        // TrackWatcherImpl::CheckForSustainedNoteTimeout(float ms), each
        // poll: each sustain held scored, and gone at its end.
        struct PollTag;
        void onPoll(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t watcher = GPR_U32(ctx, 4);
            const auto found = s_held.find(watcher);
            if (found == s_held.end() || found->second.empty())
                return;
            const EntryArgs args(ctx);
            const uint32_t player = playerOf(rdram, watcher);
            for (Held &held : found->second)
                score(rdram, ctx, runtime, player, held, args.f12);
            std::erase_if(found->second, [&](const Held &held) { return held.end <= args.f12; });
            args.restore(ctx);
        }

        // TrackWatcherImpl::CheckForPitchBend(float ms), each poll: the
        // whammy (its sound's bend and star power) on the sustain hit last,
        // as GH2 bends the one it holds. Its Player is given that one's
        // HeldNote for the call too, as Player::SetWhammyBar (0x111d88)
        // earns star power only while it holds one.
        PS2Runtime::RecompiledFunction s_pitchBend = nullptr;
        void pitchBend(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const TrackWatcherImpl watcher(rdram, GPR_U32(ctx, 4));
            const auto found = s_held.find(watcher.address);
            if (!playingCustom() || found == s_held.end() || found->second.empty())
                return s_pitchBend(rdram, ctx, runtime);
            const auto last = std::max_element(found->second.begin(), found->second.end(),
                                               [](const Held &a, const Held &b) { return a.hit < b.hit; });
            asHeldNote(rdram, playerOf(rdram, watcher.address), last->note,
                       [&]
                       {
                           watcher.setSustained(last->gem);
                           watcher.setSustainHit(last->hit);
                           callOriginal(rdram, ctx, runtime, s_addresses->checkForPitchBend, s_pitchBend);
                           watcher.setSustained(-1);
                       });
        }

        // GuitarTrackWatcherImpl::FretButtonUp(int slot): each sustain held
        // on that fret let go, a split chord's only for a lane not yet done.
        PS2Runtime::RecompiledFunction s_fretButtonUp = nullptr;
        void fretButtonUp(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t watcher = GPR_U32(ctx, 4), slot = GPR_U32(ctx, 5);
            const auto found = s_held.find(watcher);
            if (found == s_held.end() || found->second.empty() || slot >= 8u)
                return s_fretButtonUp(rdram, ctx, runtime);
            const EntryArgs args(ctx);
            const float now = watcherNow(rdram, ctx, runtime, watcher);
            std::erase_if(found->second, [&](const Held &held)
                          {
                              if (!(((held.lanes & ~done(rdram, watcher, held, now)) >> slot) & 1u))
                                  return false;
                              if (held.end > now)
                                  letGo(rdram, ctx, runtime, watcher, held, now);
                              return true;
                          });
            args.restore(ctx);
            s_fretButtonUp(rdram, ctx, runtime);
        }

        // GuitarTrackWatcherImpl::FretButtonDown(int slot): an open's
        // sustain, held with no fret, let go by any; one under later gems
        // of the player's own (Face-Off's other player's aside) held
        // whatever is pressed, as Clone Hero's.
        struct FretDownTag;
        void onFretDown(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t watcher = GPR_U32(ctx, 4);
            const auto found = s_held.find(watcher);
            if (found == s_held.end() ||
                std::none_of(found->second.begin(), found->second.end(), [](const Held &h) { return h.lanes == 0u; }))
                return;
            const EntryArgs args(ctx);
            const float now = watcherNow(rdram, ctx, runtime, watcher);
            const TrackWatcherImpl view(rdram, watcher);
            const guest::GameGemList gems = view.gems();
            std::erase_if(found->second, [&](const Held &held)
                          {
                              int next = held.gem + 1;
                              while (next < gems.size() && !view.mine(next))
                                  ++next;
                              if (held.lanes != 0u || (next < gems.size() && gems[next].ms() < held.end))
                                  return false;
                              if (held.end > now)
                                  letGo(rdram, ctx, runtime, watcher, held, now);
                              return true;
                          });
            args.restore(ctx);
        }

        // HeldNote::HeldNote(int gem, const GameGem &), as Player::Hit makes
        // one: a split chord's points (each fret's GH2's for the longest
        // lane) its lanes' own, each as long as its sustain.
        constexpr uint32_t kNoteAll = 0x0cu;
        PS2Runtime::RecompiledFunction s_heldNoteCtor = nullptr;
        void heldNoteCtor(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t note = GPR_U32(ctx, 4), game = GPR_U32(ctx, 6);
            callOriginal(rdram, ctx, runtime, s_addresses->heldNoteCtor, s_heldNoteCtor);
            const Gh2Gem *chart =
                playingCustom()
                    ? chartGem(s_hitTrack, static_cast<uint32_t>(GameGem(rdram, game).tick()), gemFrets(rdram, game))
                    : nullptr;
            if (!chart || !chart->split())
                return;
            float sum = 0.0f, longest = 0.0f;
            for (const float ms : chart->laneMs)
                sum += ms, longest = std::max(longest, ms);
            const auto frets = static_cast<float>(__builtin_popcount(gemFrets(rdram, game)));
            const auto all = static_cast<float>(load<int32_t>(rdram, note + kNoteAll));
            store<int32_t>(rdram, note + kNoteAll, static_cast<int32_t>(std::lround(all * sum / (longest * frets))));
        }

        // GuitarTrackWatcherImpl's matching of the frets held to a gem, by
        // its frets held (+0x84): Swing (strummed, given the top fret held),
        // TryToHopo and TryToFinishSwing. The frets held for sustains are
        // left out for each, those of the gem played aside, so a gem is
        // played under and beside them as though they were not held.
        template <uint32_t Addresses::*At, PS2Runtime::RecompiledFunction *Original, bool TopFret>
        void matchUnder(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const GuitarTrackWatcherImpl watcher(rdram, GPR_U32(ctx, 4));
            const uint32_t held = watcher.held();
            uint32_t sustained = 0u;
            if (playingCustom() && held != 0u && s_held.count(watcher.address))
            {
                const EntryArgs args(ctx);
                const float f14 = ctx->f[14];
                const int32_t next = closestUnplayed(rdram, ctx, runtime, watcher.address);
                const uint8_t frets = next >= 0 ? gemFrets(rdram, watcherGem(rdram, watcher.address, next)) : 0u;
                sustained = held & sustainedLanes(rdram, watcher.address, frets);
                args.restore(ctx);
                ctx->f[14] = f14;
            }
            if (sustained == 0u)
                return (*Original)(rdram, ctx, runtime);
            const uint32_t rest = held & ~sustained;
            watcher.setHeld(rest);
            if (TopFret && GPR_U32(ctx, 5) < 32u && ((sustained >> GPR_U32(ctx, 5)) & 1u) && rest != 0u)
                SET_GPR_U32(ctx, 5, 31u - static_cast<uint32_t>(__builtin_clz(rest)));
            callOriginal(rdram, ctx, runtime, s_addresses->*At, *Original);
            watcher.setHeld(watcher.held() | sustained);
        }
        PS2Runtime::RecompiledFunction s_swing = nullptr, s_tryToHopo = nullptr, s_tryToFinishSwing = nullptr;
    }

    uint32_t sustainedLanes(uint8_t *rdram, uint32_t watcher, uint8_t next)
    {
        uint32_t lanes = 0u;
        if (const auto found = s_held.find(watcher); found != s_held.end())
            for (const Held &held : found->second)
                lanes |= held.lanes;
        return lanes & ~uint32_t(next);
    }

    uint8_t heldLanes(uint8_t *rdram, uint32_t watcher, float now)
    {
        uint8_t lanes = 0u;
        if (const auto found = s_held.find(watcher); found != s_held.end())
            for (const Held &held : found->second)
                lanes |= held.lanes & ~done(rdram, watcher, held, now);
        return lanes;
    }

    uint32_t trackWatcher(uint32_t track)
    {
        const auto found = s_watchers.find(track);
        return found != s_watchers.end() ? found->second : 0u;
    }

    void forgetSustains()
    {
        s_held.clear();
        s_players.clear();
        s_watchers.clear();
    }

    void installSustainHold(PS2Runtime &runtime, const Addresses &addresses)
    {
        s_addresses = &addresses;
        EntryHook<PlayerHitTag>::install(runtime, addresses.playerHit, onPlayerHit);
        replaceKeeping(runtime, addresses.trackWatcherHitGem, &hitGem, s_hitGem);
        EntryHook<PenalizeTag>::install(runtime, addresses.playerPenalize, onPenalize);
        EntryHook<JumpTag>::install(runtime, addresses.trackWatcherJump, onJump);
        EntryHook<PollTag>::install(runtime, addresses.trackWatcherSustainTimeout, onPoll);
        replaceKeeping(runtime, addresses.checkForPitchBend, &pitchBend, s_pitchBend);
        // Over hammer_ons' own: the fret's sustains are let go before that
        // runs.
        replaceKeeping(runtime, addresses.guitarWatcherFretButtonUp, &fretButtonUp, s_fretButtonUp);
        EntryHook<FretDownTag>::install(runtime, addresses.guitarWatcherFretButtonDown, onFretDown);
        replaceKeeping(runtime, addresses.heldNoteCtor, &heldNoteCtor, s_heldNoteCtor);
        replaceKeeping(runtime, addresses.guitarWatcherSwing, &matchUnder<&Addresses::guitarWatcherSwing, &s_swing, true>,
                       s_swing);
        replaceKeeping(runtime, addresses.guitarWatcherTryToHopo,
                       &matchUnder<&Addresses::guitarWatcherTryToHopo, &s_tryToHopo, false>, s_tryToHopo);
        replaceKeeping(runtime, addresses.guitarWatcherTryToFinishSwing,
                       &matchUnder<&Addresses::guitarWatcherTryToFinishSwing, &s_tryToFinishSwing, false>,
                       s_tryToFinishSwing);
    }
}
