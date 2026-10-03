// SystemInit (retail 0x2a8320), its only caller, seeds the game's random
// numbers with SeedRand(seconds since midnight) (0x2d9cc8), so runs started
// in different seconds pick differently: a fail sequence's length, for one.

#include "dev/seed.h"

#include "hook.h"

#include "ps2_runtime_macros.h"

#include <optional>

namespace gh2::seed
{
    namespace
    {
        std::optional<int> s_seed;

        struct SeedRandTag;
        void onSeedRand(uint8_t *, R5900Context *ctx, PS2Runtime *)
        {
            SET_GPR_S32(ctx, 4, *s_seed);
        }
    }

    void fix(int seed)
    {
        s_seed = seed;
    }

    void install(PS2Runtime &runtime, const Addresses &addresses)
    {
        if (s_seed)
            EntryHook<SeedRandTag>::install(runtime, addresses.seedRand, onSeedRand);
    }
}
