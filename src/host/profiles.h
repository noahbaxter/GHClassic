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
    };

    Profile keyboardProfile();

    // The layout a controller gets with nothing in input.ini: a known guitar's,
    // else GH2's joypad layout on any SDL gamepad, else none.
    std::optional<BuiltinProfile> builtinProfile(SDL_Joystick *joystick, SDL_Gamepad *gamepad);
}
