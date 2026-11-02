#include "ui/transitions.h"

#include "guest.h"
#include "hook.h"
#include "milo/layout.h"
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"

#include <algorithm>
#include <string>
#include <vector>

namespace gh2::transitions
{
    namespace
    {
        // kStill and kSameRoom, config/menus.toml's [transitions].
#include "menu_rules.inc"

        // GHScreen's: whether its Enter and Exit turn the camera, as
        // CheckAnimateTransition sets it.
        constexpr uint32_t kAnimate = 0x4cu;

        const Addresses *s_addresses = nullptr;
        // The screen the one entering came from, or the one exiting goes to.
        uint32_t s_other = 0u;
        // The way that led to the screen now, by name, it last: a change
        // into one of these is back, and drops the rest after it.
        std::vector<std::string> s_trail;

        std::string nameOf(uint8_t *rdram, uint32_t object)
        {
            const uint32_t text = object != 0u ? load<uint32_t>(rdram, object + milo::object::kName) : 0u;
            return text != 0u ? reinterpret_cast<const char *>(getMemPtr(rdram, text)) : "";
        }

        bool still(const std::string &screen, const std::string &other)
        {
            for (const char *name : kStill)
                if (screen == name)
                    return true;
            for (const auto &room : kSameRoom)
                if ((screen == room[0] && other == room[1]) || (screen == room[1] && other == room[0]))
                    return true;
            return false;
        }

        struct EnterTag;
        void onEnter(uint8_t *, R5900Context *ctx, PS2Runtime *)
        {
            s_other = GPR_U32(ctx, 5);
        }

        // Exit comes first in a change, before the next screen's Enter, and
        // both turn the way the flag says.
        struct ExitTag;
        void onExit(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            s_other = GPR_U32(ctx, 5);
            if (s_other == 0u)
                return;
            const std::string from = nameOf(rdram, GPR_U32(ctx, 4));
            const std::string to = nameOf(rdram, s_other);
            if (s_trail.empty() || s_trail.back() != from)
                s_trail.push_back(from);
            const auto at = std::find(s_trail.begin(), s_trail.end(), to);
            const bool back = at != s_trail.end();
            if (back)
                s_trail.erase(at + 1, s_trail.end());
            else
                s_trail.push_back(to);
            store<uint32_t>(rdram, s_addresses->ghScreenForwards, back ? 0u : 1u);
        }

        // A screen still for this change does not turn, whatever its own
        // animate_transition says; the rest read theirs.
        struct CheckTag;
        bool check(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            const uint32_t screen = GPR_U32(ctx, 4);
            if (!still(nameOf(rdram, screen), nameOf(rdram, s_other)))
                return false;
            store<uint32_t>(rdram, screen + kAnimate, 0u);
            leave(ctx);
            return true;
        }

    }

    void install(PS2Runtime &runtime, const Addresses &addresses)
    {
        s_addresses = &addresses;
        EntryHook<EnterTag>::install(runtime, addresses.ghScreenEnter, onEnter);
        EntryHook<ExitTag>::install(runtime, addresses.ghScreenExit, onExit);
        Override<CheckTag>::install(runtime, addresses.ghScreenCheckAnimateTransition, check);
    }
}
