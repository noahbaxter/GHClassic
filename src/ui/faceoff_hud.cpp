// faceoff_hud: Face-Off's score meters and messages centred over each
// player's highway, at 4:3 and in widescreen (ui/dta/faceoff_hud.dta), set
// as each song's HUD enters.

#include "ui/faceoff_hud.h"

#include "dta.h"
#include "hook.h"
#include "script.h"

#include "ps2_runtime_macros.h"

namespace gh2
{
    namespace
    {
        struct HudEnterTag;
        void onHudEnter(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const EntryArgs args(ctx);
            script::run(rdram, ctx, runtime, "{faceoff_hud_enter}");
            args.restore(ctx);
        }
    }

    void installFaceoffHud(PS2Runtime &runtime, const Addresses &addresses)
    {
        script::runWhenUiReady(kFaceoffHudDta);
        EntryHook<HudEnterTag>::install(runtime, addresses.hudPanelEnter, onHudEnter);
    }
}
