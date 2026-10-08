#include "content/focus.h"

#include "content/campaigns.h"
#include "hook.h"

namespace gh2::focus
{
    namespace
    {
        struct FocusTag;
        void onSetDepthOfField(uint8_t *, R5900Context *ctx, PS2Runtime *)
        {
            if (ctx->f[15] > 0.0f && campaigns::active() == "gh1")
                ctx->f[14] = 2.0f * ctx->f[12] / ctx->f[15];
        }
    }

    void install(PS2Runtime &runtime, const Addresses &addresses)
    {
        EntryHook<FocusTag>::install(runtime, addresses.psRndSetDepthOfField, onSetDepthOfField);
    }
}
