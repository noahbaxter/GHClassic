#include "content/encores.h"

#include "content/campaigns.h"
#include "guest.h"
#include "script.h"

#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"

#include <initializer_list>
#include <set>

namespace gh2::encores
{
    namespace
    {
        constexpr uint32_t kData = 0x0u;        // CampaignState's CampaignData*
        constexpr uint32_t kItems = 0x14u;      // its items, venues among them
        constexpr uint32_t kDifficulty = 0x24u; // the career's

        const Addresses *s_addresses = nullptr;
        std::set<std::string> s_none;
        PS2Runtime::RecompiledFunction s_isEncoreSong = nullptr;
        PS2Runtime::RecompiledFunction s_isEncoreUnlockPossible = nullptr;
        PS2Runtime::RecompiledFunction s_checkUnlockVenue = nullptr;

        bool played()
        {
            return has(campaigns::active());
        }

        void answer(R5900Context *ctx, uint32_t value)
        {
            SET_GPR_U32(ctx, 2, value);
            ctx->pc = GPR_U32(ctx, 31);
        }

        // CampaignState::IsEncoreSong(Symbol) (0x131d30).
        void isEncoreSong(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (played())
                return s_isEncoreSong(rdram, ctx, runtime);
            answer(ctx, 0u);
        }

        // CampaignState::IsEncoreUnlockPossible(Symbol) (0x132b18): whether
        // the song about to be played earns the encore offer (endgame.dta).
        void isEncoreUnlockPossible(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (played())
                return s_isEncoreUnlockPossible(rdram, ctx, runtime);
            answer(ctx, 0u);
        }

        // CampaignState::CheckUnlockVenue(Symbol song) (0x1322c8) as it runs
        // on easy: after any song of the campaign's, the venue is passed once
        // enough of its songs are, and that unlocks the next, which it
        // returns.
        void checkUnlockVenue(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (played())
                return s_checkUnlockVenue(rdram, ctx, runtime);
            const uint32_t returnTo = GPR_U32(ctx, 31);
            const uint32_t state = GPR_U32(ctx, 4), song = GPR_U32(ctx, 5);
            const uint32_t data = load<uint32_t>(rdram, state + kData);
            const uint32_t difficulty = load<uint32_t>(rdram, state + kDifficulty);
            const auto call = [&](uint32_t function, std::initializer_list<uint32_t> args)
            {
                return static_cast<uint32_t>(runtime->callGuestFunction(rdram, ctx, function, args));
            };
            uint32_t unlocked = script::symbol(rdram, ctx, runtime, "");
            if (call(s_addresses->campaignDataIsStoreSong, {data, song}) == 0u)
            {
                const uint32_t venue = call(s_addresses->campaignDataGetVenueForSong, {data, song});
                const auto passed = static_cast<int32_t>(call(s_addresses->campaignStateGetNumPassedSongs, {state, venue}));
                const auto required =
                    static_cast<int32_t>(call(s_addresses->campaignDataGetRequiredSongs, {data, difficulty, venue}));
                if (passed >= required)
                    call(s_addresses->campaignItemSetPassed,
                         {call(s_addresses->campaignItemsFind, {state + kItems, venue}), 1u});
                if (call(s_addresses->campaignStateIsVenuePassed, {state, venue}) != 0u)
                {
                    const uint32_t next = call(s_addresses->campaignDataGetNextVenue, {data, venue, difficulty});
                    if (next != load<uint32_t>(rdram, s_addresses->nullStr) &&
                        call(s_addresses->campaignStateIsUnlocked, {state, next}) == 0u)
                    {
                        call(s_addresses->campaignStateSetVenueUnlocked, {state, next, 1u});
                        unlocked = next;
                    }
                }
            }
            SET_GPR_U32(ctx, 2, unlocked);
            ctx->pc = returnTo;
        }
    }

    void none(const std::string &game)
    {
        s_none.insert(game);
    }

    bool has(const std::string &game)
    {
        return s_none.count(game) == 0u;
    }

    void install(PS2Runtime &runtime, const Addresses &addresses)
    {
        s_addresses = &addresses;
        s_isEncoreSong = runtime.lookupFunction(addresses.campaignStateIsEncoreSong);
        runtime.replaceFunction(addresses.campaignStateIsEncoreSong, &isEncoreSong);
        s_isEncoreUnlockPossible = runtime.lookupFunction(addresses.campaignStateIsEncoreUnlockPossible);
        runtime.replaceFunction(addresses.campaignStateIsEncoreUnlockPossible, &isEncoreUnlockPossible);
        s_checkUnlockVenue = runtime.lookupFunction(addresses.campaignStateCheckUnlockVenue);
        runtime.replaceFunction(addresses.campaignStateCheckUnlockVenue, &checkUnlockVenue);
    }
}
