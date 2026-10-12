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
        struct StarOnTag;
        struct StarOffTag;

        void onSetCharTexture(uint8_t *, R5900Context *ctx, PS2Runtime *)
        {
            if (settings::hd(settings::kLookHighway))
                look::addBoard(GPR_U32(ctx, 5));
        }

        void onStartStarPower(uint8_t *, R5900Context *, PS2Runtime *) { look::setStarPower(true); }
        void onStopStarPower(uint8_t *, R5900Context *, PS2Runtime *) { look::setStarPower(false); }
    }

    void installTrack(PS2Runtime &runtime, const Addresses &addresses)
    {
        EntryHook<SurfaceTag>::install(runtime, addresses.charTrackSurfaceSetCharTexture, onSetCharTexture);
        EntryHook<StarOnTag>::install(runtime, addresses.playerMatcherOnStartStarpower, onStartStarPower);
        EntryHook<StarOffTag>::install(runtime, addresses.playerMatcherOnStopStarpower, onStopStarPower);
    }
}
