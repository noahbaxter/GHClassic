#pragma once

// A scenario's controllers and keys (input.h): its own devices, put in the
// device list (device.h) beside SDL's, its controllers plugged into SDL as
// real ones are, and the keys it holds. Set from any thread; synced on the
// host thread, each poll.

#include <SDL3/SDL.h>

#include <array>

namespace gh2::input
{
    // Its controllers plugged into SDL and pulled out as it asks, each
    // holding its controls; whether any came or went.
    bool syncVirtualControllers();
    // Its own devices into the list, each as it holds its controls now.
    void syncScriptedDevices();
    // The keys it holds into `keys`; whether it has held any yet, in which
    // case a hidden run has a keyboard.
    bool scriptedKeys(std::array<bool, SDL_SCANCODE_COUNT> &keys);
}
