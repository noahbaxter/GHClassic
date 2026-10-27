#pragma once

#include "host/bindings.h"

#include <SDL3/SDL.h>

#include <optional>
#include <string>

namespace gh2::input
{
    struct BuiltinProfile
    {
        std::string label; // what the log calls it
        Profile profile;
        bool gamepad = false; // GH2's joypad layout on a gamepad, else a guitar's
    };

    Profile keyboardProfile();

    // What a controller's layout is chosen by.
    struct DeviceInfo
    {
        uint16_t vendor = 0, product = 0;
        SDL_JoystickType type = SDL_JOYSTICK_TYPE_UNKNOWN;
        SDL_GamepadType gamepadType = SDL_GAMEPAD_TYPE_UNKNOWN;
        bool gamepad = false; // SDL knows its layout
    };
    DeviceInfo deviceInfo(SDL_Joystick *joystick, SDL_Gamepad *gamepad);

    // The layout a controller gets with nothing in input.ini: a known guitar's,
    // else a guitar's by SDL's type, else GH2's joypad layout on any SDL
    // gamepad, else none.
    std::optional<BuiltinProfile> builtinProfile(const DeviceInfo &info);
}
