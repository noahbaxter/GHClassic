#include "content/crowd_cards.h"

#include "content/campaigns.h"
#include "guest.h"
#include "hook.h"
#include "milo/layout.h"

namespace gh2::crowd_cards
{
    namespace
    {
        // WorldCrowd::SetFullness(float flat, float whole) (0x26bdf0), which
        // a crowd's script calls as its song starts and at each change of
        // excitement. Each crowd makes its own material, so another game's
        // keeps the blend it was made with.
        struct FullnessTag;
        void onSetFullness(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            if (campaigns::active() != "gh1")
                return;
            const uint32_t mat = load<uint32_t>(rdram, GPR_U32(ctx, 4) + milo::crowd::kCardMat);
            if (mat != 0u)
                store<uint32_t>(rdram, mat + milo::mat::kBlend, milo::mat::kBlendSrcAlpha);
        }
    }

    void install(PS2Runtime &runtime, const Addresses &addresses)
    {
        EntryHook<FullnessTag>::install(runtime, addresses.worldCrowdSetFullness, onSetFullness);
    }
}
