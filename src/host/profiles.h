#pragma once

#include "host/bindings.h"
#include "host/device_id.h"

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
    // A guitar that SDL drives as a gamepad (the CRKD Les Paul in XInput
    // mode), and what a gamepad set to guitar mode gets: frets A B Y X LB,
    // strum on the D-pad, star power on Back or D-pad left, whammy on the left
    // trigger (as YARG's CRKD layouts have it).
    Profile guitarOnGamepad();
    // What a menu reads with nothing in input.ini's [menu keyboard] or
    // [menu gamepad] (menu_layer.h): the keyboard's Enter, Space, Esc, Tab, 3
    // 4 5 and Shift, a gamepad's A B Y X LB, Start, Back and D-pad.
    Profile keyboardMenuProfile();
    Profile gamepadMenuProfile();

    // What a controller's layout is chosen by.
    struct DeviceInfo
    {
        uint16_t vendor = 0, product = 0;
        SDL_JoystickType type = SDL_JOYSTICK_TYPE_UNKNOWN;
        SDL_GamepadType gamepadType = SDL_GAMEPAD_TYPE_UNKNOWN;
        bool gamepad = false; // SDL knows its layout
        std::string name;     // as SDL names it
    };
    DeviceInfo deviceInfo(SDL_Joystick *joystick, SDL_Gamepad *gamepad);

    // The layout a controller gets with nothing in input.ini: a known guitar's,
    // else a guitar's by SDL's type, else GH2's joypad layout on any SDL
    // gamepad, else none.
    std::optional<BuiltinProfile> builtinProfile(const DeviceInfo &info);
    // The same for a controller playing as `playsAs`: a gamepad set to guitar
    // gets a guitar's layout, not the DualShock's.
    std::optional<BuiltinProfile> builtinProfile(const DeviceInfo &info, DeviceKind playsAs);
}
