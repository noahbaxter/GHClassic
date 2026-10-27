// libpad's scePadRead, answered from the host's seats.
//
// The upstream stub reads through its override state when one is set. That
// state answers every port, so it is set before each port's read: port N
// reads seat N (input.h), and a port with no seat to it reads as unplugged.
//
// Port 0 is a RedOctane guitar. GH2's config/gen/joypad.dtb classifies
//     ro_guitar  (detect (type kJoypadAnalog) (button kPad_DLeft))
// so the guitar is an analog pad with no motors that holds D-Left forever.
// With motors reported it reads as kJoypadDualShock, takes the joypad fret
// layout, and green no longer confirms in menus.

#include "host/pad.h"
#include "script.h"

#include "dev/timing.h"
#include "guest.h"
#include "hook.h"
#include "host/bindings.h"
#include "host/input.h"
#include "milo/layout.h"
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"

#include <atomic>
#include <string>

namespace ps2_stubs
{
    void setPadOverrideState(uint16_t buttons, uint8_t lx, uint8_t ly, uint8_t rx, uint8_t ry);
    void scePadRead(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
}

namespace gh2
{
    namespace
    {
        std::atomic<uint16_t> s_scripted{0}; // a scenario's presses on P1's port
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
            const int port = static_cast<int>(getRegU32(ctx, 4));
            if (port >= 0 && port < kSeats && seatPlugged(port))
            {
                // Its seat's guitar, idle until a device sits in it.
                const HostPad pad = hostPad(port);
                uint16_t pressed = pad.pressed | pad::kLeft;
                if (port == 0)
                {
                    if (timing::on())
                        timePress(pad);
                    pressed |= s_scripted.load(std::memory_order_relaxed);
                }
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

        // {seat_has <seat> <device>}: 1 while that seat reads that device, a
        // scenario's (device <name>) or a real one as the log names it.
        script::Node seatHasCommand(const script::Call &call)
        {
            return {seatHas(static_cast<int>(call.number(1)), call.symbol(2)) ? 1u : 0u, script::kInt};
        }

        // A song's screens, by name, for the keyboard's keys in it (input.h):
        // the two it is played on and the two pause menus.
        struct GotoScreenTag;
        void onGotoScreen(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            const uint32_t screen = GPR_U32(ctx, 5);
            const uint32_t text = screen != 0u ? load<uint32_t>(rdram, screen + milo::object::kName) : 0u;
            const std::string name = text != 0u ? reinterpret_cast<const char *>(getMemPtr(rdram, text)) : "";
            songScreen(name == "game_screen" || name == "practice_game_screen"  ? SongScreen::kPlaying
                       : name == "pause_screen" || name == "pract_pause_screen" ? SongScreen::kPaused
                                                                                : SongScreen::kOther);
        }
    }

    void installPad(PS2Runtime &runtime, const Addresses &addresses)
    {
        script::addCommand("pad_held", padHeld);
        script::addCommand("seat_has", seatHasCommand);
        EntryHook<GotoScreenTag>::install(runtime, addresses.uiGotoScreen, onGotoScreen);
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
