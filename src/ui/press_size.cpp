#include "ui/press_size.h"

#include "guest.h"
#include "hook.h"
#include "milo/layout.h"
#include "script.h"

#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"

#include <set>
#include <string>

namespace gh2::press_size
{
    namespace
    {
        // BandButton's: its UIComponent state, and the UI second it was
        // focused (SetState, 0x28cc88), 0 once its ease in is done.
        // DrawShowing (0x28bcb8) sizes it by its button.tnm's frame for the
        // state: 1 to 2 over a quarter second focused, 3 pressed; and colours
        // it by the state again (GetStateColor, 0x28c610).
        constexpr uint32_t kState = 0x110u;
        constexpr uint32_t kFocusedAt = 0x190u;
        constexpr uint32_t kFocused = 1u, kPressed = 3u; // UIComponent::kFocused, kSelecting
        constexpr uint32_t kObject = 0x1a0u;             // its Hmx::Object, the virtual base

        const Addresses *s_addresses = nullptr;
        PS2Runtime::RecompiledFunction s_drawShowing = nullptr;
        std::set<std::string> s_held; // by name: a panel loaded again makes its buttons anew
        uint32_t s_drawing = 0u; // a held button drawn as focused, coloured as pressed

        void drawShowing(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t button = GPR_U32(ctx, 4);
            const uint32_t name = load<uint32_t>(rdram, button + kObject + milo::object::kName);
            if (load<uint32_t>(rdram, button + kState) != kPressed || name == 0u ||
                !s_held.count(reinterpret_cast<const char *>(getMemPtr(rdram, name))))
            {
                s_drawShowing(rdram, ctx, runtime);
                return;
            }
            const float focusedAt = load<float>(rdram, button + kFocusedAt);
            store<uint32_t>(rdram, button + kState, kFocused);
            store<float>(rdram, button + kFocusedAt, 0.0f);
            s_drawing = button;
            callOriginal(rdram, ctx, runtime, s_addresses->bandButtonDrawShowing, s_drawShowing);
            s_drawing = 0u;
            store<uint32_t>(rdram, button + kState, kPressed);
            store<float>(rdram, button + kFocusedAt, focusedAt);
        }

        struct ColorTag;
        void onStateColor(uint8_t *, R5900Context *ctx, PS2Runtime *)
        {
            if (s_drawing != 0u && GPR_U32(ctx, 4) == s_drawing)
                SET_GPR_U32(ctx, 5, kPressed);
        }

        script::Node holdsSize(const script::Call &call)
        {
            s_held.insert(call.text(1));
            return {0u, script::kInt};
        }
    }

    void install(PS2Runtime &runtime, const Addresses &addresses)
    {
        s_addresses = &addresses;
        replaceKeeping(runtime, addresses.bandButtonDrawShowing, drawShowing, s_drawShowing);
        EntryHook<ColorTag>::install(runtime, addresses.bandButtonGetStateColor, onStateColor);
        script::addCommand("press_holds_size", holdsSize);
    }
}
