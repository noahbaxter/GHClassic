// libpad's scePadRead, answered from the host controller.
//
// The upstream stub reads through its override state when one is set. That
// state answers every port, so GH2 saw one controller as two pads and each
// press moved twice: port 0 has the pad and the rest read as unplugged.
//
// Port 0 is a RedOctane guitar. GH2's config/gen/joypad.dtb classifies
//     ro_guitar  (detect (type kJoypadAnalog) (button kPad_DLeft))
// so the guitar is an analog pad with no motors that holds D-Left forever.
// With motors reported it reads as kJoypadDualShock, takes the joypad fret
// layout, and green no longer confirms in menus.

#include "host/pad.h"
#include "script.h"

#include "dev/timing.h"
#include "host/bindings.h"
#include "host/input.h"
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"

#include <atomic>

namespace ps2_stubs
{
    void setPadOverrideState(uint16_t buttons, uint8_t lx, uint8_t ly, uint8_t rx, uint8_t ry);
    void scePadRead(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
}

namespace gh2
{
    namespace
    {
        std::atomic<uint16_t> s_scripted{0};
        std::atomic<uint64_t> s_scriptedAt{0}; // --timing: when a scripted press was set

        // --timing: a press this read is the first to see, the player's or
        // a script's.
        void timePress(const HostPad &pad)
        {
            static uint32_t seenCount = 0;
            static uint64_t seenScripted = 0;
            timing::padRead();
            const uint64_t now = timing::now(), scriptedAt = s_scriptedAt.load(std::memory_order_relaxed);
            if (pad.pressCount != seenCount)
            {
                timing::span(timing::kPollToRead, pad.pressPollNs, now);
                timing::pressRead(pad.pressEventNs, now);
            }
            else if (scriptedAt != seenScripted)
            {
                timing::pressRead(scriptedAt, now);
            }
            seenCount = pad.pressCount;
            seenScripted = scriptedAt;
        }

        void padRead(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t returnTo = GPR_U32(ctx, 31);
            if (getRegU32(ctx, 4) == 0u)
            {
                const HostPad pad = hostPad();
                if (timing::on())
                    timePress(pad);
                const uint16_t pressed = pad.pressed | s_scripted.load(std::memory_order_relaxed) | pad::kLeft;
                // The stub wants buttons active low.
                ps2_stubs::setPadOverrideState(static_cast<uint16_t>(~pressed), pad.lx, pad.ly, pad.rx, pad.ry);
                ps2_stubs::scePadRead(rdram, ctx, runtime);
            }
            else
            {
                setReturnS32(ctx, 0);
            }
            ctx->pc = returnTo;
        }

        // scePadInfoAct(port, slot, actno, term): no actuators, on any port.
        void padInfoAct(uint8_t *, R5900Context *ctx, PS2Runtime *)
        {
            setReturnS32(ctx, 0);
            ctx->pc = GPR_U32(ctx, 31);
        }
    }

    uint16_t padButton(const std::string &name)
    {
        if (name == "up" || name == "down")
            return input::kActions[name == "up" ? input::kStrumUp : input::kStrumDown].button;
        if (name == "left" || name == "right")
            return name == "left" ? pad::kLeft : pad::kRight;
        for (const input::ActionInfo &action : input::kActions)
            if (name == action.name)
                return action.button;
        return 0u;
    }

    namespace
    {
        // {pad_held up|down|left|right|<fret colour>}: 1 while that is held,
        // for menus that repeat on a hold. The UI gets only presses, and
        // gets blue and orange both as kPad_Square.
        script::Node padHeld(const script::Call &call)
        {
            const uint16_t bit = padButton(call.symbol(1));
            const uint16_t pressed = hostPad().pressed | s_scripted.load(std::memory_order_relaxed);
            return {bit != 0u && (pressed & bit) != 0u ? 1u : 0u, script::kInt};
        }
    }

    void installPad(PS2Runtime &runtime, const Addresses &addresses)
    {
        script::addCommand("pad_held", padHeld);
        runtime.replaceFunction(addresses.scePadRead, padRead);
        runtime.replaceFunction(addresses.scePadInfoAct, padInfoAct);
    }

    void setScriptedPad(uint16_t pressed)
    {
        if (timing::on() && (pressed & ~s_scripted.load(std::memory_order_relaxed)))
            s_scriptedAt.store(timing::now(), std::memory_order_relaxed);
        s_scripted.store(pressed, std::memory_order_relaxed);
    }
}
