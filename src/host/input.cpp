// The keyboard and every connected controller, read through SDL into one PS2
// guitar.
//
// Each poll reads every device into its actions (sdl_devices.cpp, the
// keyboard through its layout in layouts.cpp). All devices play at once; an
// action is held when any device holds it.
//
// The actions land on the RedOctane guitar's buttons (see pad.cpp for why the
// pad is one): frets R2 Circle Triangle Cross Square, the guitar slots in
// config/beatmatch_controller.dta; strum on the D-pad; star power on Select
// (force_mercury); tilt on L2, the mercury_switch GuitarController defaults
// to when the guitar config names none (kPad_L2 is 0, 0x2364d8); whammy on
// the left stick's Y pushed up, the half GetWhammyBar reads (0x236718).

#include "input.h"

#include "dev/timing.h"
#include "host/bindings.h"
#include "host/device.h"
#include "host/layouts.h"
#include "host/owner.h"
#include "host/sdl_devices.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
#include <mutex>

namespace gh2
{
    namespace
    {
        using namespace input;

        bool s_open = false;

        std::mutex s_mutex;
        HostPad s_pad;
        bool s_flying = false;
        HostFly s_fly, s_scriptedFly;
        uint64_t s_pressEventNs = 0; // the host thread's own

        HostPad padOf(const std::array<float, kActionCount> &held)
        {
            HostPad pad;
            for (int a = 0; a < kActionCount; ++a)
                if (held[a] > kHeld)
                    pad.pressed |= kActions[a].button;
            pad.ly = static_cast<uint8_t>(std::lround(128.0f * (1.0f - held[kWhammy])));
            return pad;
        }
    }

    void openInput()
    {
        // The controller keeps playing while the window is behind the
        // terminal that launched it.
        SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
        if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD))
        {
            std::cerr << "[input] init: " << SDL_GetError() << std::endl;
            return;
        }
        loadLayouts();
        s_open = true;
        openDevices();
        claimControllers();
    }

    void pollInput(bool devicesChanged)
    {
        if (!s_open)
            return;
        if (devicesChanged)
            openDevices();
        const bool *keys = SDL_GetKeyboardState(nullptr);
        std::array<float, kActionCount> held{};
        // A window without the controllers reads none.
        if (ownsControllers())
        {
            readSdlDevices();
            play(keyboardLayout(), Snapshot{}, keys, held);
            for (Device &d : devices())
            {
                nameControls(d);
                play(d.profile, d.now, keys, held);
            }
        }
        HostPad pad = padOf(held);

        // Read every poll, so the travel from before a flight is not its
        // first step.
        float mouseX = 0.0f, mouseY = 0.0f;
        SDL_GetRelativeMouseState(&mouseX, &mouseY);
        std::lock_guard<std::mutex> lock(s_mutex);
        pad.pressCount = s_pad.pressCount;
        pad.pressEventNs = s_pad.pressEventNs;
        pad.pressPollNs = s_pad.pressPollNs;
        if (timing::on() && (pad.pressed & ~s_pad.pressed))
        {
            ++pad.pressCount;
            pad.pressPollNs = timing::now();
            // An event from before the last poll is another press's.
            pad.pressEventNs = s_pressEventNs > s_pad.pressPollNs ? s_pressEventNs : 0u;
            if (pad.pressEventNs != 0u)
                timing::span(timing::kEventToPoll, pad.pressEventNs, pad.pressPollNs);
        }
        s_pad = pad;
        if (s_flying)
        {
            s_fly.right = (keys[SDL_SCANCODE_D] ? 1.0f : 0.0f) - (keys[SDL_SCANCODE_A] ? 1.0f : 0.0f);
            s_fly.forward = (keys[SDL_SCANCODE_W] ? 1.0f : 0.0f) - (keys[SDL_SCANCODE_S] ? 1.0f : 0.0f);
            s_fly.fast = keys[SDL_SCANCODE_LSHIFT];
            s_fly.rollLeft = keys[SDL_SCANCODE_Q];
            s_fly.rollRight = keys[SDL_SCANCODE_E];
            s_fly.mouseX += mouseX;
            s_fly.mouseY += mouseY;
        }
    }

    void notePressEvent(uint64_t ns)
    {
        s_pressEventNs = ns;
    }

    void closeInput()
    {
        if (!s_open)
            return;
        closeDevices();
        SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
        s_open = false;
        std::lock_guard<std::mutex> lock(s_mutex);
        s_pad = HostPad{};
    }

    HostPad hostPad()
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        return s_pad;
    }

    void setFlying(bool flying)
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        s_flying = flying;
        s_fly = HostFly{};
    }

    bool flying()
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        return s_flying;
    }

    HostFly takeFly()
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        HostFly fly = s_fly;
        fly.right = std::clamp(fly.right + s_scriptedFly.right, -1.0f, 1.0f);
        fly.forward = std::clamp(fly.forward + s_scriptedFly.forward, -1.0f, 1.0f);
        fly.mouseX += s_scriptedFly.mouseX;
        fly.mouseY += s_scriptedFly.mouseY;
        fly.fast |= s_scriptedFly.fast;
        fly.rollLeft |= s_scriptedFly.rollLeft;
        fly.rollRight |= s_scriptedFly.rollRight;
        s_fly.mouseX = s_fly.mouseY = 0.0f;
        s_scriptedFly.mouseX = s_scriptedFly.mouseY = 0.0f;
        return fly;
    }

    void setScriptedFly(const HostFly &fly)
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        s_scriptedFly = fly;
    }
}
