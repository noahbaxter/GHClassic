#include "ui/screen_loads.h"

#include "guest.h"
#include "hook.h"
#include "milo/layout.h"
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "runtime/ee_scheduler.h"
#include "script.h"

#include <algorithm>
#include <cstring>
#include <map>
#include <string>
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

        // The scene a panel's (file ...) names now, as UIPanel::Load
        // (0x20e2a0) evaluates it: its type's (file ...), "" for none. Some
        // are worked out each load: the song list's by setlist, the venue
        // picker's by game, the character screen's by mode.
        std::string fileOf(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t panel)
        {
            const uint32_t object = load<uint32_t>(rdram, panel);
            const uint32_t type = object != 0u ? load<uint32_t>(rdram, object + 4u) : 0u;
            if (type == 0u)
                return "";
            static const uint32_t s_file = script::symbol(rdram, ctx, runtime, "file");
            const auto file = static_cast<uint32_t>(
                runtime->callGuestFunction(rdram, ctx, s_addresses->dataArrayFindArray, {type, s_file, 0u}));
            if (file == 0u)
                return "";
            const auto text = static_cast<uint32_t>(runtime->callGuestFunction(
                rdram, ctx, s_addresses->dataNodeStr, {load<uint32_t>(rdram, file) + 8u, file}));
            return text != 0u ? reinterpret_cast<const char *>(getMemPtr(rdram, text)) : "";
        }
        std::map<uint32_t, std::string> s_files; // each panel's, as it last loaded

        // A kept panel whose file names another scene now (a setlist picked
        // since) is let go before it is loaded again, so it loads that.
        struct LoadTag;
        void onCheckLoad(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t panel = GPR_U32(ctx, 4);
            if (s_meta == 0u && isMeta(rdram, panel))
                s_meta = panel;
            const int32_t loads = load<int32_t>(rdram, panel + kLoads);
            const auto kept = std::find(s_kept.begin(), s_kept.end(), panel);
            if (loads != 0 && (loads != 1 || kept == s_kept.end()))
                return;
            const EntryArgs args(ctx);
            std::string file = fileOf(rdram, ctx, runtime, panel);
            if (loads == 1 && file != s_files[panel])
            {
                s_kept.erase(kept);
                runtime->callGuestFunction(rdram, ctx, s_addresses->uiPanelCheckUnload, {panel}, s_checkUnload);
            }
            s_files[panel] = std::move(file);
            args.restore(ctx);
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
        // {ghc_preload <screen>}: its panels loading now, while another
        // screen is still up, held until {ghc_unpreload <screen>}, once it
        // has taken them itself. {ghc_preloaded <screen>}: whether they all
        // have. A change into it then waits on nothing, where it would
        // otherwise load behind the last screen gone.
        script::Node panels(const script::Call &call, uint32_t function)
        {
            const uint32_t screen = call.object(1);
            if (screen == 0u)
                return {};
            return {static_cast<uint32_t>(call.runtime->callGuestFunction(call.rdram, call.ctx, function, {screen})),
                    script::kInt};
        }
        script::Node preload(const script::Call &call)
        {
            panels(call, s_addresses->uiScreenLoadPanels);
            return {};
        }
        script::Node preloaded(const script::Call &call)
        {
            return panels(call, s_addresses->uiScreenArePanelsLoaded);
        }
        script::Node unpreload(const script::Call &call)
        {
            panels(call, s_addresses->uiScreenUnloadPanels);
            return {};
        }

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
        script::addCommand("ghc_preload", preload);
        script::addCommand("ghc_preloaded", preloaded);
        script::addCommand("ghc_unpreload", unpreload);
        Unpaced<LoadMgrPollTag>::install(runtime, addresses.loadMgrPoll);
        Unpaced<ScreenEnterTag>::install(runtime, addresses.ghScreenEnter);
        Unpaced<ScreenExitTag>::install(runtime, addresses.ghScreenExit);
    }
}
