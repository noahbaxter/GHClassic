#pragma once

// A device's raw controls at one moment, and what a layout plays on them.

#include "host/bindings.h"

#include <SDL3/SDL.h>

#include <array>
#include <cstdint>
#include <vector>

namespace gh2::input
{
    // A device's raw controls at one moment: a joystick's, the same as SDL's
    // gamepad layout reads them when it has one, and its accelerometer in
    // m/s^2 when that is turned on. Every source reads off one.
    struct Snapshot
    {
        std::vector<bool> buttons;
        std::vector<uint8_t> hats;
        std::vector<float> axes;
        std::vector<bool> padButtons; // by SDL_GamepadButton
        std::vector<float> padAxes;   // by SDL_GamepadAxis
        std::vector<float> accel;
    };

    // A joystick's own controls, without its gamepad layout.
    Snapshot readJoystick(SDL_Joystick *joystick);

    // How far a source is pressed, 0 to 1. Keys read the same on every
    // device, so a controller's section can bind keys too; none without
    // `keys`.
    float amount(const Source &s, const Snapshot &now, const bool *keys);
    // Each action's amount on `profile`: the most of its sources.
    void play(const Profile &profile, const Snapshot &now, const bool *keys, std::array<float, kActionCount> &held);
}
