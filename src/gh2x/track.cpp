#include "gh2x/track.h"

#include "hook.h"
#include "ps2_runtime_macros.h"
#include "render/look.h"
#include "settings/settings.h"

namespace gh2::gh2x
{
    namespace
    {
        struct SurfaceTag;

        void onSetCharTexture(uint8_t *, R5900Context *ctx, PS2Runtime *)
        {
            if (settings::get(settings::kModels) == settings::kX360)
                look::addBoard(GPR_U32(ctx, 5));
        }
    }

    void installTrack(PS2Runtime &runtime, const Addresses &addresses)
    {
        EntryHook<SurfaceTag>::install(runtime, addresses.charTrackSurfaceSetCharTexture, onSetCharTexture);
    }
}
