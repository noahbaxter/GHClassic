#pragma once

// Every device a player can hold but the keyboard, whatever reads it: SDL's
// controllers (sdl_devices.cpp). Each backend opens its own into the one list
// and fills their raw controls each poll; what they play reads only those.
// The host thread's own.

#include "host/bindings.h"
#include "host/capture.h"

#include <SDL3/SDL.h>

#include <string>
#include <vector>

namespace gh2::input
{
    struct Device
    {
        std::string name; // as the log names it
        // Its input.ini section, by SDL's name.
        std::string sectionName;
        Profile builtin; // its layout before input.ini
        Profile profile; // what it plays
        Snapshot now;    // its raw controls this poll
        // SDL's, for one it opened.
        SDL_Joystick *joystick = nullptr;
        SDL_Gamepad *gamepad = nullptr; // when SDL knows its layout
    };

    std::vector<Device> &devices();
}
