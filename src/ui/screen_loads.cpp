#include "ui/screen_loads.h"

#include "guest.h"
#include "hook.h"
#include "milo/layout.h"
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "runtime/ee_scheduler.h"

#include <algorithm>
#include <cstring>
#include <vector>

namespace gh2::screen_loads
{
    namespace
    {
        // UIPanel's: how many screens hold it loaded, CheckLoad's count up
        // and CheckUnload's down, unloaded at 0. Its first word its
        // Hmx::Object.
        constexpr uint32_t kLoads = 0x30u;

        const Addresses *s_addresses = nullptr;
        PS2Runtime::RecompiledFunction s_checkUnload = nullptr;
        uint32_t s_meta = 0u;
        std::vector<uint32_t> s_kept;

        bool isMeta(uint8_t *rdram, uint32_t panel)
        {
            const uint32_t object = load<uint32_t>(rdram, panel);
            const uint32_t name = object != 0u ? load<uint32_t>(rdram, object + milo::object::kName) : 0u;
            return name != 0u && std::strcmp(reinterpret_cast<const char *>(getMemPtr(rdram, name)), "meta") == 0;
        }

        struct LoadTag;
        void onCheckLoad(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            if (s_meta == 0u && isMeta(rdram, GPR_U32(ctx, 4)))
                s_meta = GPR_U32(ctx, 4);
        }

        // The next screen has loaded its panels by the time the last lets go
        // of its own (UIScreen::Enter, 0x212f70), meta first of each: meta
        // still loaded then, the menus stay.
        struct UnloadTag;
        bool onCheckUnload(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t panel = GPR_U32(ctx, 4);
            if (load<int32_t>(rdram, panel + kLoads) != 1)
                return false;
            if (panel == s_meta)
            {
                std::vector<uint32_t> kept;
                kept.swap(s_kept);
                const EntryArgs args(ctx);
                for (const uint32_t other : kept)
                    runtime->callGuestFunction(rdram, ctx, s_addresses->uiPanelCheckUnload, {other}, s_checkUnload);
                args.restore(ctx);
                return false;
            }
            if (s_meta == 0u || load<int32_t>(rdram, s_meta + kLoads) < 1)
                return false;
            if (std::find(s_kept.begin(), s_kept.end(), panel) == s_kept.end())
                s_kept.push_back(panel);
            leave(ctx);
            return true;
        }

        // Only the outermost of nested ones unpaced, which moves the
        // scheduler's deadlines once.
        int s_unpacedDepth = 0;
        template <typename Tag>
        struct Unpaced
        {
            inline static PS2Runtime::RecompiledFunction s_original = nullptr;

            static void run(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
            {
                struct Depth
                {
                    Depth() { ++s_unpacedDepth; }
                    ~Depth() { --s_unpacedDepth; }
                } depth;
                if (s_unpacedDepth > 1)
                    return s_original(rdram, ctx, runtime);
                const EeScheduler::UnpacedScope unpaced(runtime->eeScheduler());
                s_original(rdram, ctx, runtime);
            }

            static void install(PS2Runtime &runtime, uint32_t address)
            {
                s_original = runtime.lookupFunction(address);
                runtime.replaceFunction(address, &run);
            }
        };
        struct LoadMgrPollTag;
        struct ScreenEnterTag;
        struct ScreenExitTag;
    }

    void install(PS2Runtime &runtime, const Addresses &addresses)
    {
        s_addresses = &addresses;
        s_checkUnload = runtime.lookupFunction(addresses.uiPanelCheckUnload);
        EntryHook<LoadTag>::install(runtime, addresses.uiPanelCheckLoad, onCheckLoad);
        Override<UnloadTag>::install(runtime, addresses.uiPanelCheckUnload, onCheckUnload);
        Unpaced<LoadMgrPollTag>::install(runtime, addresses.loadMgrPoll);
        Unpaced<ScreenEnterTag>::install(runtime, addresses.ghScreenEnter);
        Unpaced<ScreenExitTag>::install(runtime, addresses.ghScreenExit);
    }
}
