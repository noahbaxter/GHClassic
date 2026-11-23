#include "gameplay/engine/no_autoplay.h"

#include "hook.h"
#include "ps2_runtime.h"

#include "ps2_runtime_macros.h"

namespace gh2
{
    namespace
    {
        // BeatMatcher::SetAutoplay(bool).
        struct SetAutoplayTag;
        void onSetAutoplay(uint8_t *, R5900Context *ctx, PS2Runtime *)
        {
            SET_GPR_U32(ctx, 5, 0u);
        }
    }

    void installNoAutoplay(PS2Runtime &runtime, const Addresses &addresses)
    {
        EntryHook<SetAutoplayTag>::install(runtime, addresses.beatMatcherSetAutoplay, onSetAutoplay);
    }
}
