// Boot straight to the main menu. App::App (retail 0x100240) shows each
// boot logo, runs a batch of inits, then spins on a Timer read off Count
// until three seconds have passed since the logo (0x1006d0, 0x100858,
// 0x100a28). Each spin follows one call, made from nowhere else: MetaInit,
// GHUtl::Init and ProgressiveScanCheck. Moving Count three seconds on at
// their entry ends the spin after it on its first check; the inits before
// it have all returned. The logos themselves are never loaded or drawn.
// Past the save's load, fast_boot.dta skips the intro movie and the
// press-start splash.

#include "fast_boot.h"

#include "dta.h"
#include "hook.h"
#include "script.h"

#include "ps2_runtime_macros.h"
#include "runtime/ee_scheduler.h"

namespace gh2::fast_boot
{
    namespace
    {
        bool s_enabled = false;

        void skipSpin(uint8_t *, R5900Context *, PS2Runtime *runtime)
        {
            runtime->setCop0Count(runtime->cop0Count() + static_cast<uint32_t>(3u * EeScheduler::kEeClockHz));
        }

        // Splash::Show(path), retail 0x2139f0: loads the logo's milo, waits
        // out the last one, draws it twice and deletes it. Nothing outlives
        // the call but the Splash's own timer, which only its Wait reads.
        void skipShow(uint8_t *, R5900Context *ctx, PS2Runtime *)
        {
            ctx->pc = GPR_U32(ctx, 31);
        }

        struct MetaInitTag;
        struct GhUtlInitTag;
        struct ScanCheckTag;
    }

    void enable()
    {
        s_enabled = true;
    }

    void install(PS2Runtime &runtime, const Addresses &addresses)
    {
        if (!s_enabled)
            return;
        EntryHook<MetaInitTag>::install(runtime, addresses.metaInit, skipSpin);
        EntryHook<GhUtlInitTag>::install(runtime, addresses.ghUtlInit, skipSpin);
        EntryHook<ScanCheckTag>::install(runtime, addresses.progressiveScanCheck, skipSpin);
        runtime.replaceFunction(addresses.splashShow, skipShow);
        script::runWhenUiReady(kFastBootDta);
    }
}
