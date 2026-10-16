// note_streak: a message at each milestone streak, from 50 notes
// (ui/dta/note_streak.dta). Its sinks go on each song's player matchers as
// the HUD enters, where Deluxe 2.0's own HUD script adds them: every game's
// HUD is a HudPanel, whatever its script.

#include "streak/note_streak.h"

#include "content/locale.h"
#include "dta.h"
#include "hook.h"
#include "script.h"

#include "ps2_runtime_macros.h"

#include <string>

namespace gh2
{
    namespace
    {
        struct HudEnterTag;
        void onHudEnter(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const EntryArgs args(ctx);
            script::run(rdram, ctx, runtime, "{note_streak_enter}");
            args.restore(ctx);
        }
    }

    void installNoteStreak(PS2Runtime &runtime, const Addresses &addresses)
    {
        script::runWhenUiReady(kNoteStreakDta);
        // Deluxe 2.0's text (ui/eng/locale_milo.dta there).
        for (const char *count : {"note_streak_0", "note_streak_1", "note_streak_band"})
            locale::addExpression(count, std::string("{sprint $") + count + " \" NOTE STREAK!\"}");
        EntryHook<HudEnterTag>::install(runtime, addresses.hudPanelEnter, onHudEnter);
    }
}
