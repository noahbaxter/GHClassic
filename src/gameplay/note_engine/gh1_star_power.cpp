// GH1's player (Player, StarPower, GH1 0x118158 on): GH2's with these
// differences, each put back here.
//
// - A bad strum (an overstrum or wrong fret) while a star phrase's first
//   gem is in the window loses the phrase, even if the gem is then hit.
//   GH2 lets that one gem off, so hitting it still starts the phrase
//   (Player::Penalize, GH1 0x1182c0).
// - A hit scores at the multiplier from before it, so the 10th, 20th and
//   30th gem of a streak score one multiplier lower than in GH2
//   (Player::UpdateMultiplier, GH1 0x118bc8, kept from one hit to the
//   next).
// - A star phrase that star power is on for only stops being tracked.
//   Once star power runs out, hitting any later gem of it starts it
//   again, and the rest still wins it. In GH2 star power loses the phrase
//   (StarPower::Hit, MissOrPass, GH1 0x11e158 0x11e608).
// - The first gem at or past a phrase's end is in no phrase. So when two
//   phrases sit back to back, the second loses its first gem
//   (SongDB::SetupStarpower, GH1 0x10cf00).

#include "gameplay/note_engine/gh1.h"

#include "guest/beatmatch.h"
#include "guest/song_db.h"
#include "guest/star_power.h"
#include "hook.h"

#include "ps2_runtime_macros.h"

namespace gh2::gh1_rules
{
    namespace
    {
        using guest::GameGem;
        using guest::SongDb;
        using guest::SongDbTrack;
        using guest::StarPhrase;
        using guest::StarPower;
        using guest::Vector;

        const Addresses *s_addresses = nullptr;

        // GH1 has no phrase start: Penalize always passes the gem on to
        // MissOrPass, Pass leaves it there, and StarPower::Hit takes a phrase
        // up from state 0 alone, which onStarPowerHit does.
        struct IsStarPhraseStartTag;
        bool onIsStarPhraseStart(uint8_t *, R5900Context *ctx, PS2Runtime *)
        {
            const uint32_t ra = GPR_U32(ctx, 31);
            if (ra != s_addresses->penalizeAsksStarPhraseStart && ra != s_addresses->passAsksStarPhraseStart &&
                ra != s_addresses->starPowerHitAsksStarPhraseStart)
                return false;
            SET_GPR_U32(ctx, 2, 0u);
            leave(ctx);
            return true;
        }

        // GH1 adds points at the multiplier Player last worked out, before
        // this hit's streak. Only set under GH1's.
        uint32_t s_hitPlayer = 0u;
        uint32_t s_hitMultiplier = 0u;

        struct PlayerHitTag;
        void onPlayerHit(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            s_hitPlayer = 0u; // so the call below is the original's
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
            if (GPR_U32(ctx, 4) != s_hitPlayer)
                return false;
            SET_GPR_U32(ctx, 2, s_hitMultiplier);
            leave(ctx);
            return true;
        }

        // Player::Hit calls StarPower::Hit last, after the hit's points are
        // in, so it also lets go of the multiplier onPlayerHit held.
        struct StarPowerHitTag;
        bool onStarPowerHit(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            s_hitPlayer = 0u;
            const StarPower star(rdram, GPR_U32(ctx, 4));
            if (!star.enabled())
                return false;
            if (star.active())
            {
                star.setState(StarPower::kNone);
                leave(ctx);
                return true;
            }
            if (star.state() == StarPower::kNone)
            {
                const EntryArgs args(ctx);
                runtime->callGuestFunction(rdram, ctx, s_addresses->songDbIsInStarPhrase,
                                           {load<uint32_t>(rdram, s_addresses->theSongDb), star.track(), args.a[1]});
                if (GPR_U32(ctx, 2) != 0u)
                    star.setState(StarPower::kInPhrase); // any gem of it, not only its first
                args.restore(ctx);
            }
            return false;
        }

        struct MissOrPassTag;
        bool onMissOrPass(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            const StarPower star(rdram, GPR_U32(ctx, 4));
            if (!star.enabled() || !star.active())
                return false;
            star.setState(StarPower::kNone);
            leave(ctx);
            return true;
        }

        // GH2's setup first, then the first gem at or past each phrase's end
        // is taken out of the next phrase, and the gem after it becomes that
        // phrase's first. Set while the original runs, so the call to it
        // below reaches GH2's code and not this hook.
        bool s_settingUpPhrases = false;

        struct SetupStarPhrasesTag;
        bool onSetupStarPhrases(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (s_settingUpPhrases)
                return false;
            const EntryArgs args(ctx);
            const uint32_t db = args.a[0], track = args.a[1];
            s_settingUpPhrases = true;
            runtime->callGuestFunction(rdram, ctx, s_addresses->songDbSetupStarPhrases, {db, track});
            s_settingUpPhrases = false;
            runtime->callGuestFunction(rdram, ctx, s_addresses->songDbGetGems, {db, track});
            const Vector<GameGem> gems(rdram, GPR_U32(ctx, 2));
            const SongDbTrack info = SongDb(rdram, db).track(track);
            const Vector<StarPhrase> phrases = info.phrases();
            int p = 0;
            for (int i = 0; i < gems.size(); ++i)
            {
                if (p >= phrases.size() || gems[i].tick() < phrases[p].end())
                    continue;
                ++p;
                if ((info.flags(i) & SongDbTrack::kIn) == 0u)
                    continue;
                info.setFlags(i, 0u);
                if (i + 1 < gems.size() && (info.flags(i + 1) & SongDbTrack::kIn) != 0u)
                    info.setFlags(i + 1, info.flags(i + 1) | SongDbTrack::kFirst);
            }
            args.restore(ctx);
            leave(ctx);
            return true;
        }
    }

    void installStarPower(PS2Runtime &runtime, const Addresses &addresses)
    {
        s_addresses = &addresses;
        Override<IsStarPhraseStartTag>::install(runtime, addresses.songDbIsStarPhraseStart, gh1Only<onIsStarPhraseStart>);
        EntryHook<PlayerHitTag>::install(runtime, addresses.playerHit, gh1Only<onPlayerHit>);
        EntryHook<PlayerPollTag>::install(runtime, addresses.playerPoll, onPlayerPoll);
        Override<GetMultiplierTag>::install(runtime, addresses.performerGetMultiplier, gh1Only<onGetMultiplier>);
        Override<StarPowerHitTag>::install(runtime, addresses.starPowerHit, gh1Only<onStarPowerHit>);
        Override<MissOrPassTag>::install(runtime, addresses.starPowerMissOrPass, gh1Only<onMissOrPass>);
        Override<SetupStarPhrasesTag>::install(runtime, addresses.songDbSetupStarPhrases, gh1Only<onSetupStarPhrases>);
    }
}
