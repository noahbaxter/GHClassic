// note_engine: whose rules judge a song, set as a career is entered and from
// quickplay's pause menu, and taken as the next song loads. GH2's are
// retail's own: every hook here hands GH2's calls to the original. GH1's
// (TrackBeatMatcher, GH1 debug 0x2805f0) are GH2's guitar watcher with these
// differences, each put back here:
//
// - A gem is a HOPO when it is one fret, not the last gem's frets, and at
//   most 159 ticks after it (GameGemList::WillBeNoStrum, GH1 0x277e58).
//   GH2's must share no fret with the last gem and be under the song's
//   hopo_threshold, 170 unless the song says.
// - A gem under 121 ticks long scores no sustain (GH1 0x27f2a8). GH2's
//   parser cuts at 160 and under (0x245ff8).
// - No hammer-on or pull-off counts once every fret has been let go, until
//   the next strum, even one BeatMatcher::Swing drops inside its 52 ms
//   (GuitarController::OnMsg, GH1 0x278120).
// - A hammer-on or pull-off aims at the nearest gem, played or not, from
//   slop - 60 ms early (40) where GH2's aims at the nearest unplayed one
//   from slop early (100). Both reach 60 past slop late. It may come in
//   the frame of a hit, which GH2's may not (TryToHopo, GH1 0x281650).
// - A gem may be hammered once the gem before it is played. GH2's also
//   wants that gem the last hit, which after a practice jump none is.
// - A wrong pull-off locks the gem, so it must be strummed, as a wrong
//   hammer-on does in both.
// - A strum whose nearest gem is played is an overstrum, unless it is the
//   gem just hammered and still within slop. GH2 strums the gem after it
//   instead (Swing, GH1 0x280b38).
// - Every miss counts. GH2 drops one within 50 ms of the last, and names
//   the gem after a missed one only when it was the last passed, where
//   GH1's is the last hit or passed (SendMiss, GH1 0x282160).
// - A HOPO-able gem is passed at slop, not 60 later, and a gem waiting
//   for its frets is passed as any other, then hit if they come in time
//   (CheckForPasses, GH1 0x2818e8). GH2 holds the pass (GemCanBePassed).
// - A strum missing its frets misses when its 60 ms run out, ahead of the
//   poll's passes (TrackBeatMatcher::Poll, GH1 0x280a10), and past a
//   hammered gem as any other. GH2 lets that one go.
// - scoring.dta's crowd ranges on medium and hard and the stars' top
//   thresholds (config/gen/scoring.dta on each disc), in the live config.
//
// And on the player's side (Player, StarPower, GH1 0x118158 on):
//
// - A miss on a star phrase's first gem loses the phrase as on any other
//   (Player::Penalize, GH1 0x1182c0). GH2 keeps it for a strum to win back.
// - Star power on through part of a phrase only stops it being followed,
//   and the next gem of it hit once star power is out takes it up again
//   (StarPower::Hit, MissOrPass, GH1 0x11e158 0x11e608). GH2 loses it.
// - A hit scores at the multiplier before it: the 10th, 20th and 30th of
//   a streak at the one below (Player::UpdateMultiplier, GH1 0x118bc8,
//   kept from one hit to the next).
// - The gem after a phrase's end is in no phrase, so of two phrases end
//   to end the second loses its first gem (SongDB::SetupStarpower, GH1
//   0x10cf00).
//
// Kept GH2's: the song's sync offset in every window.

#include "note_engine.h"

#include "content/campaigns.h"
#include "guest.h"
#include "hook.h"
#include "save/save.h"
#include "script.h"

#include "ps2_runtime_macros.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <iostream>
#include <optional>
#include <unordered_map>

namespace gh2
{
    namespace
    {
        // TrackWatcherImpl (ctor, retail 0x248100).
        constexpr uint32_t kPlayer = 0x00u;     // whose gems, for a gem's player mask
        constexpr uint32_t kGems = 0x04u;       // GameGemList *
        constexpr uint32_t kPlayed = 0x08u;     // GameGemInfoList *: an int a gem, 1 once hit
        constexpr uint32_t kParent = 0x0cu;     // TrackWatcherParent *, the BeatMatcher
        constexpr uint32_t kSlop = 0x10u;       // the hit window each side, ms
        constexpr uint32_t kLastHit = 0x14u;    // gem last hit
        constexpr uint32_t kCaptureEnd = 0x18u; // gems before it are not judged
        constexpr uint32_t kSync = 0x28u;       // ms added to now
        constexpr uint32_t kLastPassed = 0x4cu; // CheckForPasses' gem: every one to it is done with
        constexpr uint32_t kLastMissMs = 0x54u; // SendMiss sends none within 50 ms of it
        constexpr uint32_t kVtable = 0x78u;
        // GuitarTrackWatcherImpl (ctor, 0x24d400).
        constexpr uint32_t kHeld = 0x84u;          // frets held
        constexpr uint32_t kHopoGem = 0x88u;       // gem last hit by a hammer-on or pull-off
        constexpr uint32_t kLastHitMs = 0x94u;     // TryToHopo hits nothing at it
        constexpr uint32_t kPendingAfterHopo = 0xa4u;
        constexpr uint32_t kLeniency = 0xa8u;      // 60 ms
        // GameGemList: the gems' vector. A GameGem is 0x10 bytes.
        constexpr uint32_t kListBegin = 0x08u, kListEnd = 0x0cu;
        constexpr uint32_t kGemTick = 0x04u, kGemSlots = 0x0cu, kGemFlags = 0x0du, kGemPlayers = 0x0eu;
        // MultiGemInfo, as SongParser::OnGemEnd (0x245ee8) fills it.
        constexpr uint32_t kInfoSlots = 0x04u, kInfoTick = 0x10u, kInfoTicks = 0x14u, kInfoNoSustain = 0x1cu;

        constexpr int32_t kGh1HopoTicks = 159;
        constexpr int32_t kGh1SustainTicks = 121;

        const Addresses *s_addresses = nullptr;
        std::string s_engine; // empty until first asked: quickplay's

        bool gh1()
        {
            return noteEngine() == "gh1";
        }
        // Each guitar watcher's: a strum since every fret was last let go.
        std::unordered_map<uint32_t, bool> s_strummed;

        // A function replaced by one of ours that answers some calls itself
        // and hands the rest to the original. One that answers sets the
        // return and leaves for $ra.
        template <typename Tag>
        struct Override
        {
            using Callback = bool (*)(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);

            static void install(PS2Runtime &runtime, uint32_t address, Callback answer)
            {
                s_original = runtime.lookupFunction(address);
                s_answer = answer;
                runtime.replaceFunction(address, &run);
            }

        private:
            static void run(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
            {
                if (!s_answer(rdram, ctx, runtime))
                    s_original(rdram, ctx, runtime);
            }

            inline static PS2Runtime::RecompiledFunction s_original = nullptr;
            inline static Callback s_answer = nullptr;
        };

        void leave(R5900Context *ctx)
        {
            ctx->pc = GPR_U32(ctx, 31);
        }

        struct Gems
        {
            uint8_t *rdram;
            uint32_t watcher;
            uint32_t begin;
            int count;

            Gems(uint8_t *r, uint32_t w) : rdram(r), watcher(w)
            {
                const uint32_t list = load<uint32_t>(r, w + kGems);
                begin = load<uint32_t>(r, list + kListBegin);
                count = static_cast<int>((load<uint32_t>(r, list + kListEnd) - begin) / 0x10u);
            }

            uint32_t at(int i) const { return begin + static_cast<uint32_t>(i) * 0x10u; }
            float ms(int i) const { return load<float>(rdram, at(i)); }

            // GameGemList::ClosestMarkerIdx (0x24d1c8): a tie is the earlier gem's.
            int closest(float to) const
            {
                int i = 0;
                while (i < count && ms(i) < to)
                    ++i;
                if (i == 0)
                    return 0;
                if (i == count)
                    return count - 1;
                return std::fabs(to - ms(i)) < std::fabs(to - ms(i - 1)) ? i : i - 1;
            }

            bool played(int i) const
            {
                const uint32_t played = load<uint32_t>(rdram, load<uint32_t>(rdram, watcher + kPlayed) + 4u);
                return load<int32_t>(rdram, played + static_cast<uint32_t>(i) * 4u) != 0;
            }

            // Hit already, or another player's (TrackWatcherImpl::Playable, 0x249088).
            bool done(int i) const
            {
                const uint8_t players = load<uint8_t>(rdram, at(i) + kGemPlayers);
                const int32_t player = load<int32_t>(rdram, watcher + kPlayer);
                return played(i) || (players != 0u && ((players >> player) & 1u) == 0u);
            }
        };

        // BeatMatcher::GetNow (0x38ccc0) through the watcher's parent.
        float nowOf(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t watcher)
        {
            const uint32_t parent = load<uint32_t>(rdram, watcher + kParent);
            const uint32_t vtable = load<uint32_t>(rdram, parent);
            const uint32_t self = parent + static_cast<uint32_t>(static_cast<int32_t>(load<int16_t>(rdram, vtable + 0x10u)));
            runtime->callGuestFunction(rdram, ctx, load<uint32_t>(rdram, vtable + 0x14u), {self});
            return ctx->f[0];
        }

        struct WillBeNoStrumTag;
        bool onWillBeNoStrum(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            if (!gh1())
                return false;
            const uint32_t list = GPR_U32(ctx, 4), info = GPR_U32(ctx, 5);
            const int32_t ticks = load<int32_t>(rdram, info + kInfoTicks);
            store<int32_t>(rdram, info + kInfoNoSustain, ticks < kGh1SustainTicks ? 1 : 0);
            bool hopo = false;
            const uint32_t begin = load<uint32_t>(rdram, list + kListBegin), end = load<uint32_t>(rdram, list + kListEnd);
            if (begin != end)
            {
                const uint32_t last = end - 0x10u;
                const uint32_t slots = load<uint32_t>(rdram, info + kInfoSlots);
                hopo = slots != load<uint8_t>(rdram, last + kGemSlots) && std::popcount(slots) == 1 &&
                       load<int32_t>(rdram, info + kInfoTick) - load<int32_t>(rdram, last + kGemTick) <= kGh1HopoTicks;
            }
            SET_GPR_U32(ctx, 2, hopo ? 1u : 0u);
            leave(ctx);
            return true;
        }

        struct CtorTag;
        void onCtor(uint8_t *, R5900Context *ctx, PS2Runtime *)
        {
            s_strummed[GPR_U32(ctx, 4)] = false;
        }

        // The watcher a BeatMatcher's swings go to, its track's, as
        // TrackWatcher::Swing (0x247f68) finds it.
        uint32_t swingWatcher(uint8_t *rdram, uint32_t beatMatcher)
        {
            const uint32_t watchers = load<uint32_t>(rdram, beatMatcher + 0x38u);
            const uint32_t track = load<uint32_t>(rdram, watchers + 4u * load<uint32_t>(rdram, beatMatcher + 0xa0u));
            const uint32_t impl = load<uint32_t>(rdram, track);
            const uint32_t vtable = load<uint32_t>(rdram, impl + kVtable);
            return impl + static_cast<uint32_t>(static_cast<int32_t>(load<int16_t>(rdram, vtable + 0x38u)));
        }

        // GH1's controller marks a strum before BeatMatcher::Swing's 52 ms
        // can drop it.
        struct BeatMatcherSwingTag;
        void onBeatMatcherSwing(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            if (GPR_U32(ctx, 6) != 0u)
                s_strummed[swingWatcher(rdram, GPR_U32(ctx, 4))] = true;
        }

        // GH1 times a strum out before it passes gems.
        struct PollTag;
        void onPoll(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t watcher = GPR_U32(ctx, 4);
            if (!gh1() || load<uint32_t>(rdram, watcher + kVtable) != s_addresses->guitarWatcherVtable)
                return;
            const EntryArgs args(ctx);
            runtime->callGuestFunction(rdram, ctx, s_addresses->guitarWatcherCheckForFretTimeout, {watcher});
            args.restore(ctx);
        }

        struct SendMissTag;
        void onSendMiss(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            if (!gh1())
                return;
            const uint32_t watcher = GPR_U32(ctx, 4);
            store<float>(rdram, watcher + kLastMissMs, -10000.0f);
            const int32_t gem = static_cast<int32_t>(GPR_U32(ctx, 6));
            const int32_t resolved = std::max(load<int32_t>(rdram, watcher + kLastHit), load<int32_t>(rdram, watcher + kLastPassed));
            if (gem != -1 && gem == resolved && gem + 1 < Gems(rdram, watcher).count)
                SET_GPR_U32(ctx, 6, static_cast<uint32_t>(gem + 1));
        }

        struct GemCanBePassedTag;
        bool onGemCanBePassed(uint8_t *, R5900Context *ctx, PS2Runtime *)
        {
            if (!gh1())
                return false;
            SET_GPR_U32(ctx, 2, 1u);
            leave(ctx);
            return true;
        }

        struct CanHopoTag;
        bool onCanHopo(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            if (!gh1())
                return false;
            const Gems gems(rdram, GPR_U32(ctx, 4));
            const int g = static_cast<int32_t>(GPR_U32(ctx, 5));
            const bool can = (load<uint8_t>(rdram, gems.at(g) + kGemFlags) & 1u) != 0u && g > 0 && gems.played(g - 1);
            SET_GPR_U32(ctx, 2, can ? 1u : 0u);
            leave(ctx);
            return true;
        }

        struct FretButtonUpTag;
        void onFretButtonUp(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            const uint32_t watcher = GPR_U32(ctx, 4);
            if ((load<uint32_t>(rdram, watcher + kHeld) & ~(1u << GPR_U32(ctx, 5))) == 0u)
                s_strummed[watcher] = false;
        }

        struct SwingTag;
        bool onSwing(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const EntryArgs args(ctx);
            const uint32_t watcher = args.a[0];
            const Gems gems(rdram, watcher);
            if (!gh1() || gems.count == 0)
                return false;
            const float now = nowOf(rdram, ctx, runtime, watcher);
            const float at = now + load<float>(rdram, watcher + kSync);
            const int c = gems.closest(at);
            const bool inSlop = std::fabs(at - gems.ms(c)) <= load<float>(rdram, watcher + kSlop);
            if (gems.done(c) && (c != load<int32_t>(rdram, watcher + kHopoGem) || !inSlop) &&
                c >= load<int32_t>(rdram, watcher + kCaptureEnd))
            {
                ctx->f[12] = now;
                runtime->callGuestFunction(rdram, ctx, s_addresses->trackWatcherOnMiss, {watcher, args.a[1], static_cast<uint32_t>(c)});
                leave(ctx);
                return true;
            }
            args.restore(ctx);
            return false;
        }

        struct TryToHopoTag;
        bool onTryToHopo(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            const uint32_t watcher = GPR_U32(ctx, 4);
            const Gems gems(rdram, watcher);
            if (!gh1() || gems.count == 0)
                return false;
            const float at = ctx->f[12] + load<float>(rdram, watcher + kSync);
            const int c = gems.closest(at);
            const float early = load<float>(rdram, watcher + kLeniency) - load<float>(rdram, watcher + kSlop);
            if (!s_strummed[watcher] || gems.done(c) || at - gems.ms(c) < early)
            {
                leave(ctx);
                return true;
            }
            store<float>(rdram, watcher + kLastHitMs, -1000.0f); // read nowhere else
            SET_GPR_U32(ctx, 6, 1u); // isPress, so a wrong pull-off locks the gem too
            return false;
        }

        struct SlopTag;
        bool onSlop(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            if (!gh1())
                return false;
            const uint32_t watcher = GPR_U32(ctx, 4);
            ctx->f[0] = load<float>(rdram, watcher + kSlop) - load<float>(rdram, watcher + kSync);
            leave(ctx);
            return true;
        }

        struct FretTimeoutTag;
        void onCheckForFretTimeout(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            if (gh1())
                store<int32_t>(rdram, GPR_U32(ctx, 4) + kPendingAfterHopo, 0);
        }

        // StarPower, as Player makes it.
        constexpr uint32_t kStarEnabled = 0x28u, kStarUsing = 0x2cu, kStarTrack = 0x48u;
        constexpr uint32_t kStarState = 0x58u; // 0 none, 1 phrase missed, 2 in one, 3 captured
        // SongDB +8: each track's 0x30 bytes, its phrases' vector (start and
        // end ticks) first and its gems' phrase flags (bit0 first, bit1 in
        // one, bit2 last) at +0x20.
        constexpr uint32_t kTrackPhrases = 0x00u, kTrackStarFlags = 0x20u;

        // GH1 has no phrase start: Penalize always passes the gem on to
        // MissOrPass, Pass leaves it there, and StarPower::Hit takes a phrase
        // up from state 0 alone, which onStarPowerHit does.
        struct IsStarPhraseStartTag;
        bool onIsStarPhraseStart(uint8_t *, R5900Context *ctx, PS2Runtime *)
        {
            const uint32_t ra = GPR_U32(ctx, 31);
            if (!gh1() || (ra != s_addresses->penalizeAsksStarPhraseStart && ra != s_addresses->passAsksStarPhraseStart &&
                           ra != s_addresses->starPowerHitAsksStarPhraseStart))
                return false;
            SET_GPR_U32(ctx, 2, 0u);
            leave(ctx);
            return true;
        }

        // GH1 adds points at the multiplier Player last worked out, before
        // this hit's streak.
        uint32_t s_hitPlayer = 0u;
        uint32_t s_hitMultiplier = 0u;

        struct PlayerHitTag;
        void onPlayerHit(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            s_hitPlayer = 0u;
            if (!gh1())
                return;
            const EntryArgs args(ctx);
            runtime->callGuestFunction(rdram, ctx, s_addresses->performerGetMultiplier, {args.a[0]});
            s_hitMultiplier = GPR_U32(ctx, 2);
            s_hitPlayer = args.a[0];
            args.restore(ctx);
        }

        struct PlayerPollTag;
        void onPlayerPoll(uint8_t *, R5900Context *, PS2Runtime *)
        {
            s_hitPlayer = 0u;
        }

        struct GetMultiplierTag;
        bool onGetMultiplier(uint8_t *, R5900Context *ctx, PS2Runtime *)
        {
            if (s_hitPlayer == 0u || GPR_U32(ctx, 4) != s_hitPlayer)
                return false;
            SET_GPR_U32(ctx, 2, s_hitMultiplier);
            leave(ctx);
            return true;
        }

        // Player::Hit's last call: its points are in.
        struct StarPowerHitTag;
        bool onStarPowerHit(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            s_hitPlayer = 0u;
            const uint32_t star = GPR_U32(ctx, 4);
            if (!gh1() || load<uint32_t>(rdram, star + kStarEnabled) == 0u)
                return false;
            if (load<uint32_t>(rdram, star + kStarUsing) != 0u)
            {
                store<int32_t>(rdram, star + kStarState, 0);
                leave(ctx);
                return true;
            }
            if (load<int32_t>(rdram, star + kStarState) == 0)
            {
                const EntryArgs args(ctx);
                runtime->callGuestFunction(rdram, ctx, s_addresses->songDbIsInStarPhrase,
                                           {load<uint32_t>(rdram, s_addresses->theSongDb), load<uint32_t>(rdram, star + kStarTrack), args.a[1]});
                if (GPR_U32(ctx, 2) != 0u)
                    store<int32_t>(rdram, star + kStarState, 2); // any gem of it, not only its first
                args.restore(ctx);
            }
            return false;
        }

        struct MissOrPassTag;
        bool onMissOrPass(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            const uint32_t star = GPR_U32(ctx, 4);
            if (!gh1() || load<uint32_t>(rdram, star + kStarEnabled) == 0u || load<uint32_t>(rdram, star + kStarUsing) == 0u)
                return false;
            store<int32_t>(rdram, star + kStarState, 0);
            leave(ctx);
            return true;
        }

        // GH2's flags, then for GH1 each gem that moved them past a
        // phrase's end out of the next one.
        bool s_settingUpPhrases = false;

        struct SetupStarPhrasesTag;
        bool onSetupStarPhrases(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (!gh1() || s_settingUpPhrases)
                return false;
            const EntryArgs args(ctx);
            const uint32_t db = args.a[0], track = args.a[1];
            s_settingUpPhrases = true;
            runtime->callGuestFunction(rdram, ctx, s_addresses->songDbSetupStarPhrases, {db, track});
            s_settingUpPhrases = false;
            runtime->callGuestFunction(rdram, ctx, s_addresses->songDbGetGems, {db, track});
            const uint32_t gems = GPR_U32(ctx, 2);
            const uint32_t info = load<uint32_t>(rdram, db + 8u) + track * 0x30u;
            const uint32_t phrases = load<uint32_t>(rdram, info + kTrackPhrases);
            const uint32_t phraseCount = (load<uint32_t>(rdram, info + kTrackPhrases + 4u) - phrases) / 8u;
            const uint32_t flags = load<uint32_t>(rdram, info + kTrackStarFlags);
            const uint32_t first = load<uint32_t>(rdram, gems);
            const uint32_t count = (load<uint32_t>(rdram, gems + 4u) - first) / 0x10u;
            uint32_t p = 0u;
            for (uint32_t i = 0u; i < count; ++i)
            {
                const int32_t tick = load<int32_t>(rdram, first + i * 0x10u + kGemTick);
                if (p >= phraseCount || tick < load<int32_t>(rdram, phrases + p * 8u + 4u))
                    continue;
                ++p;
                if ((load<uint8_t>(rdram, flags + i) & 2u) == 0u)
                    continue;
                store<uint8_t>(rdram, flags + i, 0u);
                if (i + 1u < count && (load<uint8_t>(rdram, flags + i + 1u) & 2u) != 0u)
                    store<uint8_t>(rdram, flags + i + 1u, load<uint8_t>(rdram, flags + i + 1u) | 1u);
            }
            args.restore(ctx);
            leave(ctx);
            return true;
        }

        // GH1's numbers where GH2's scoring.dta differs: crowd (range low
        // high) on medium 0.6 and hard 0.65, where GH2's are 0.5 and 0.6, and
        // every difficulty's star thresholds (2.0 3.0), where GH2's top one
        // is 2.8. Scoring keeps the live config's array (+0x24) and looks
        // these up as it goes (Scoring::GetCrowdConfig 0x11c400,
        // GetNumStars 0x11c4c8), so they are written there in place.
        // A difficulty's array starts on its number, the kDifficulty macro
        // the file names it by: easy 0 to expert 3.
        struct ScoringValue
        {
            std::array<const char *, 3> path; // under scoring, to the difficulties; null ends it
            int difficulty;
            const char *last; // under the difficulty's, or null for its own
            int at;
            float gh1;
        };
        constexpr ScoringValue kScoring[] = {
            {{"default_config", "crowd", nullptr}, 1, "range", 1, 0.6f},
            {{"default_config", "crowd", nullptr}, 2, "range", 1, 0.65f},
            {{"star_ratings", "thresholds", nullptr}, 0, nullptr, 2, 3.0f},
            {{"star_ratings", "thresholds", nullptr}, 1, nullptr, 2, 3.0f},
            {{"star_ratings", "thresholds", nullptr}, 2, nullptr, 2, 3.0f},
            {{"star_ratings", "thresholds", nullptr}, 3, nullptr, 2, 3.0f},
        };
        std::string s_scoringEngine = "gh2"; // whose numbers the live config holds
        std::optional<std::array<float, std::size(kScoring)>> s_gh2Scoring;

        // A DataArray: its nodes (+0), each a value and a type, their count
        // (+8). The array under `array` starting on that symbol, or with no
        // symbol on that int.
        uint32_t child(uint8_t *rdram, uint32_t array, const char *key, int number = 0)
        {
            const int size = array ? load<int16_t>(rdram, array + 8u) : 0;
            for (int i = 0; i < size; ++i)
            {
                const uint32_t node = load<uint32_t>(rdram, array) + 8u * static_cast<uint32_t>(i);
                if (load<uint32_t>(rdram, node + 4u) != script::kArray)
                    continue;
                const uint32_t found = load<uint32_t>(rdram, node);
                if (load<int16_t>(rdram, found + 8u) <= 0)
                    continue;
                const uint32_t first = load<uint32_t>(rdram, found);
                const uint32_t type = load<uint32_t>(rdram, first + 4u), value = load<uint32_t>(rdram, first);
                if (key ? type == script::kSymbol &&
                              std::strcmp(reinterpret_cast<const char *>(getMemPtr(rdram, value)), key) == 0
                        : type == script::kInt && static_cast<int32_t>(value) == number)
                    return found;
            }
            return 0u;
        }

        // CrowdRating::Configure, as each song's players are made: the live
        // config takes the engine's scoring first.
        struct ConfigureTag;
        void onCrowdConfigure(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (s_scoringEngine == noteEngine())
                return;
            const EntryArgs args(ctx);
            const uint32_t scoring = child(rdram, static_cast<uint32_t>(runtime->callGuestFunction(rdram, ctx, s_addresses->systemConfig, {})), "scoring");
            args.restore(ctx);
            std::array<uint32_t, std::size(kScoring)> nodes{};
            for (size_t i = 0; i < std::size(kScoring); ++i)
            {
                uint32_t array = scoring;
                for (const char *key : kScoring[i].path)
                    if (key)
                        array = child(rdram, array, key);
                array = child(rdram, array, nullptr, kScoring[i].difficulty);
                if (kScoring[i].last)
                    array = child(rdram, array, kScoring[i].last);
                if (array == 0u || load<int16_t>(rdram, array + 8u) <= kScoring[i].at)
                {
                    std::cerr << "[note_engine] no scoring " << kScoring[i].path[1] << " for difficulty "
                              << kScoring[i].difficulty << " in the config; scoring stays " << s_scoringEngine << "'s"
                              << std::endl;
                    s_scoringEngine = noteEngine();
                    return;
                }
                nodes[i] = load<uint32_t>(rdram, array) + 8u * static_cast<uint32_t>(kScoring[i].at);
            }
            if (!s_gh2Scoring)
            {
                s_gh2Scoring.emplace();
                for (size_t i = 0; i < nodes.size(); ++i)
                    (*s_gh2Scoring)[i] = load<uint32_t>(rdram, nodes[i] + 4u) == script::kFloat
                                             ? load<float>(rdram, nodes[i])
                                             : static_cast<float>(load<int32_t>(rdram, nodes[i]));
            }
            for (size_t i = 0; i < nodes.size(); ++i)
            {
                store<float>(rdram, nodes[i], gh1() ? kScoring[i].gh1 : (*s_gh2Scoring)[i]);
                store<uint32_t>(rdram, nodes[i] + 4u, script::kFloat);
            }
            s_scoringEngine = noteEngine();
        }

        //   {note_engine}              the engine judging now
        //   {note_engine set <engine>} quickplay's from here on, and the
        //                              careers' it holds with it; TRUE, or
        //                              FALSE in a career, whose engine stays
        script::Node command(const script::Call &call)
        {
            if (call.size() < 2)
                return {script::symbol(call.rdram, call.ctx, call.runtime, noteEngine()), script::kSymbol};
            const std::string engine = call.size() > 2 ? call.symbol(2) : "";
            if (call.symbol(1) != "set" ||
                std::find(noteEngines().begin(), noteEngines().end(), engine) == noteEngines().end())
            {
                std::cerr << "[note_engine] usage: {note_engine}, {note_engine set gh2|gh1}" << std::endl;
                return {};
            }
            if (campaigns::career())
                return {0u, script::kInt};
            save::switchEngine(engine, call.rdram, call.ctx, call.runtime);
            save::keepQuickplayEngine(engine);
            return {1u, script::kInt};
        }
    }

    const std::vector<std::string> &noteEngines()
    {
        static const std::vector<std::string> engines = {"gh2", "gh1"};
        return engines;
    }

    const std::string &noteEngine()
    {
        if (s_engine.empty())
            s_engine = save::quickplayEngine();
        return s_engine;
    }

    void setNoteEngine(const std::string &engine)
    {
        if (std::find(noteEngines().begin(), noteEngines().end(), engine) == noteEngines().end())
        {
            std::cerr << "[note_engine] no engine " << engine << std::endl;
            return;
        }
        s_engine = engine;
        std::cout << "[note_engine] " << engine << std::endl;
    }

    void installNoteEngine(PS2Runtime &runtime, const Addresses &addresses)
    {
        s_addresses = &addresses;
        script::addCommand("note_engine", command);
        EntryHook<ConfigureTag>::install(runtime, addresses.crowdRatingConfigure, onCrowdConfigure);
        Override<WillBeNoStrumTag>::install(runtime, addresses.willBeNoStrum, onWillBeNoStrum);
        EntryHook<CtorTag>::install(runtime, addresses.guitarWatcherCtor, onCtor);
        EntryHook<BeatMatcherSwingTag>::install(runtime, addresses.beatMatcherSwing, onBeatMatcherSwing);
        EntryHook<PollTag>::install(runtime, addresses.trackWatcherPoll, onPoll);
        EntryHook<SendMissTag>::install(runtime, addresses.trackWatcherSendMiss, onSendMiss);
        EntryHook<FretButtonUpTag>::install(runtime, addresses.guitarWatcherFretButtonUp, onFretButtonUp);
        Override<SwingTag>::install(runtime, addresses.guitarWatcherSwing, onSwing);
        Override<GemCanBePassedTag>::install(runtime, addresses.guitarWatcherGemCanBePassed, onGemCanBePassed);
        Override<TryToHopoTag>::install(runtime, addresses.guitarWatcherTryToHopo, onTryToHopo);
        Override<CanHopoTag>::install(runtime, addresses.guitarWatcherCanHopo, onCanHopo);
        Override<SlopTag>::install(runtime, addresses.guitarWatcherSlop, onSlop);
        EntryHook<FretTimeoutTag>::install(runtime, addresses.guitarWatcherCheckForFretTimeout, onCheckForFretTimeout);
        Override<IsStarPhraseStartTag>::install(runtime, addresses.songDbIsStarPhraseStart, onIsStarPhraseStart);
        EntryHook<PlayerHitTag>::install(runtime, addresses.playerHit, onPlayerHit);
        EntryHook<PlayerPollTag>::install(runtime, addresses.playerPoll, onPlayerPoll);
        Override<GetMultiplierTag>::install(runtime, addresses.performerGetMultiplier, onGetMultiplier);
        Override<StarPowerHitTag>::install(runtime, addresses.starPowerHit, onStarPowerHit);
        Override<MissOrPassTag>::install(runtime, addresses.starPowerMissOrPass, onMissOrPass);
        Override<SetupStarPhrasesTag>::install(runtime, addresses.songDbSetupStarPhrases, onSetupStarPhrases);
    }
}
