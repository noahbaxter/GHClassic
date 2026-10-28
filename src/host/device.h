#pragma once

// Every device a player can hold but the keyboard, whatever reads it: SDL's
// controllers (sdl_devices.cpp) and a scenario's (scripted_devices.cpp).
// Each backend opens its own into the one list and fills their raw controls
// each poll; what they play, log and show reads only those. The host
// thread's own.

#include "host/bindings.h"
#include "host/capture.h"
#include "host/device_id.h"

#include <SDL3/SDL.h>

#include <cstdint>
#include <set>
#include <string>
#include <vector>

namespace gh2::input
{
    struct Device
    {
        DeviceId id;
        std::string name;  // as the log names it
        std::string label; // what a player is shown: its layout's, else its name
        // What it plays as, its section's mode line over what it was
        // detected as.
        DeviceKind kind = DeviceKind::kController, detected = DeviceKind::kController;
        // Its input.ini section, by SDL's name and GUID, and whether a
        // change to it stays in memory: a scenario's is not the player's.
        std::string sectionName, sectionGuid;
        bool keepInMemory = false;
        Profile builtin; // its layout before input.ini
        Profile profile; // what it plays
        Snapshot now;    // its raw controls this poll
        Snapshot rest;   // at rest, for the Controllers screen
        // Each axis as first read, and the controls the log has named, so a
        // player's log says what each of theirs does.
        std::vector<float> logRest;
        std::set<std::string> named;
        uint64_t openedMs = 0;
        // A scenario's controller attached to SDL as a virtual joystick
        // (input.h), its section kept in memory.
        bool isVirtual = false;
        // SDL's, for one it opened.
        SDL_Joystick *joystick = nullptr;
        SDL_Gamepad *gamepad = nullptr; // when SDL knows its layout
    };

    std::vector<Device> &devices();
    Device *deviceById(const DeviceId &id);

    // The first time each of a device's controls is pressed, or its axis
    // pushed half way from where it first read, the log names it and what
    // it plays: what a player's log needs to say why a control does nothing.
    void nameControls(Device &d);
}
