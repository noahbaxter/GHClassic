#pragma once

// What each guitar action is bound to, per device, and the file that holds it.
//
// A source is one physical control. Each reads as an amount from 0 to 1, and
// counts as held past half:
//
//     key 1                  a keyboard key, by SDL scancode name ("Right Shift")
//     button 0               a joystick button
//     hat 0 up               a joystick hat direction
//     axis 2 -0.11 0.70      a joystick axis, from its rest value to its full one
//     pad a                  an SDL gamepad button, by SDL's mapping name (a, b,
//                            x, y, back, start, leftshoulder, dpup...)
//     pad axis righty 0 -1   an SDL gamepad axis (leftx ... righttrigger), rest then full
//
// An action takes any number of sources, comma separated, and is held when
// any of them is. Whammy takes the largest amount.

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace gh2::input
{
    enum Action
    {
        kGreen,
        kRed,
        kYellow,
        kBlue,
        kOrange,
        kStrumUp,
        kStrumDown,
        kStart,
        kStarPower,
        kTilt,
        kWhammy,
        kActionCount,
    };

    struct Source
    {
        enum Kind
        {
            kKey,
            kButton,
            kHat,
            kAxis,
            kPadButton,
            kPadAxis,
        };
        Kind kind = kKey;
        int index = 0;      // scancode, button, hat, axis, SDL_GamepadButton or SDL_GamepadAxis
        uint8_t hat = 0;    // SDL_HAT_* bit
        float rest = 0.0f;  // axes: the value untouched
        float full = 1.0f;  // axes: the value fully pressed
    };

    std::optional<Source> parseSource(const std::string &text);
    std::string formatSource(const Source &source);

    // Each action's sources. An action with none is unbound.
    using Profile = std::array<std::vector<Source>, kActionCount>;

    // input.ini: [keyboard], then one [device "<SDL joystick name>"] per
    // controller. A section lists only the actions it changes; the rest keep
    // the built-in profile's. Comments take whole lines, starting ; or #.
    struct BindingFile
    {
        std::map<std::string, std::map<Action, std::vector<Source>>> sections; // by section name
    };

    std::string keyboardSection();
    std::string deviceSection(const std::string &name);

    // Missing is empty; a line that does not parse is reported and skipped.
    BindingFile loadBindingFile(const std::string &path);
    // Replaces one section, keeping the rest of the file as it was.
    bool writeSection(const std::string &path, const std::string &section,
                      const std::map<Action, std::vector<Source>> &bindings);

    // `base` with every action the section names replaced.
    Profile overlay(Profile base, const std::map<Action, std::vector<Source>> *section);

    // Where input.ini lives: the user data directory.
    std::string bindingFilePath();
}
