#include "content/tour_or_higher.h"

#include "hook.h"
#include "ps2_runtime_macros.h"

namespace gh2
{
    namespace
    {
        constexpr uint32_t kExpert = 3u;

        struct PassedTag;
        struct FiveStarsTag;

        // Campaign::AllSongsPassed(Difficulty) and AllSongsFiveStars, retail
        // 0x12d890 and 0x12d8d8: asked of the first difficulty from the one
        // named that holds, or of the one named.
        template <uint32_t Addresses::*Function>
        void onAsked(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t asked = GPR_U32(ctx, 5);
            if ((asked & kOrHigher) == 0u)
                return;
            const EntryArgs args(ctx);
            const uint32_t named = asked & ~static_cast<uint32_t>(kOrHigher);
            uint32_t holds = named;
            for (uint32_t d = named; d <= kExpert; ++d)
                if (static_cast<uint32_t>(runtime->callGuestFunction(rdram, ctx, kSlus21447.*Function, {args.a[0], d})) != 0u)
                {
                    holds = d;
                    break;
                }
            args.restore(ctx);
            SET_GPR_U32(ctx, 5, holds);
        }
    }

    void installTourOrHigher(PS2Runtime &runtime, const Addresses &addresses)
    {
        EntryHook<PassedTag>::install(runtime, addresses.allSongsPassed, onAsked<&Addresses::allSongsPassed>);
        EntryHook<FiveStarsTag>::install(runtime, addresses.allSongsFiveStars, onAsked<&Addresses::allSongsFiveStars>);
    }
}
