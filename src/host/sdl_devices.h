#pragma once

// The controllers SDL sees, open in the device list (device.h), each with the
// layout it plays: its built-in one (profiles.cpp) with its section of
// input.ini laid over it. The host thread's own.

#include "host/bindings.h"
#include "host/device.h"

#include <SDL3/SDL.h>

#include <string>
#include <vector>

namespace gh2::input
{
    // SDL reports every joystick already plugged in as added, so the first
    // pump would reopen the ones just opened; only a changed set reopens.
    void openDevices();
    void closeDevices();
    // Each one's raw controls into its snapshot.
    void readSdlDevices();
    // Its accelerometer turned on, which sends nothing until it is.
    void enableAccel(Device &d);

    // A control as a player calls it, for the Controllers screen: a
    // gamepad's by its own labels (Cross and L1 on a PlayStation pad, A and
    // LB on the rest), a stick by its direction, a key by its name; several
    // joined with " / ".
    std::string shown(const SDL_Gamepad *gamepad, const Source &s);
    std::string joined(const SDL_Gamepad *gamepad, const std::vector<Source> &sources);
}
