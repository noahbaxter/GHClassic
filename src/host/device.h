#pragma once

// Every device a player can hold but the keyboard, whatever reads it: SDL's
// controllers (sdl_devices.cpp). Each backend opens its own into the one list
// and fills their raw controls each poll; what they play and log reads only
// those. The host thread's own.

#include "host/bindings.h"
#include "host/capture.h"

#include <SDL3/SDL.h>

#include <cstdint>
#include <set>
#include <string>
#include <vector>

namespace gh2::input
{
    struct Device
    {
        std::string name; // as the log names it
        // Its input.ini section, by SDL's name and GUID.
        std::string sectionName, sectionGuid;
        Profile builtin; // its layout before input.ini
        Profile profile; // what it plays
        Snapshot now;    // its raw controls this poll
        // Each axis as first read, and the controls the log has named, so a
        // player's log says what each of theirs does.
        std::vector<float> logRest;
        std::set<std::string> named;
        uint64_t openedMs = 0;
        // SDL's, for one it opened.
        SDL_Joystick *joystick = nullptr;
        SDL_Gamepad *gamepad = nullptr; // when SDL knows its layout
    };

    std::vector<Device> &devices();

    // The first time each of a device's controls is pressed, or its axis
    // pushed half way from where it first read, the log names it and what
    // it plays: what a player's log needs to say why a control does nothing.
    void nameControls(Device &d);
}
