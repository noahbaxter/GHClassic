// The game's widescreen option, held by the host instead of the save.
//
// Options::SyncVideoOptions (0x10db80) passes mWideScreen (+0x38) to
// Options::SetWideScreen (0x10de58), which calls TheRnd->SetAspect with 2
// for 16:9 or 1 for 4:3: Rnd::YRatio then squeezes every camera's projection
// and the host shows the picture at that aspect. Its callers are
// Campaign::Load (the save load at boot) and the options DTA handler, so the
// flag goes in on entry and a save cannot turn it back.

#include "settings/video_options.h"

#include "guest.h"
#include "hook.h"
#include "settings/settings.h"
#include "ps2_runtime_macros.h"

namespace gh2
{
    namespace
    {
        constexpr uint32_t kWideScreen = 0x38u; // Options::mWideScreen

        struct SyncTag;

        void onSync(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            store<uint32_t>(rdram, GPR_U32(ctx, 4) + kWideScreen, settings::get(settings::kWidescreen) ? 1u : 0u);
        }
    }

    void installVideoOptions(PS2Runtime &runtime, const Addresses &addresses)
    {
        EntryHook<SyncTag>::install(runtime, addresses.optionsSyncVideo, onSync);
    }
}
