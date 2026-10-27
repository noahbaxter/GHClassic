#include "host/sdl_devices.h"

#include "host/layouts.h"
#include "host/profiles.h"

#include <iostream>

namespace gh2::input
{
    namespace
    {
        // Its raw controls, and its gamepad layout's.
        Snapshot read(const Device &d)
        {
            Snapshot s = readJoystick(d.joystick);
            if (!d.gamepad)
                return s;
            for (int b = 0; b < SDL_GAMEPAD_BUTTON_COUNT; ++b)
                s.padButtons.push_back(SDL_GetGamepadButton(d.gamepad, static_cast<SDL_GamepadButton>(b)));
            for (int a = 0; a < SDL_GAMEPAD_AXIS_COUNT; ++a)
                s.padAxes.push_back(axisValue(SDL_GetGamepadAxis(d.gamepad, static_cast<SDL_GamepadAxis>(a))));
            return s;
        }
    }

    void closeDevices()
    {
        for (Device &d : devices())
        {
            if (d.gamepad)
                SDL_CloseGamepad(d.gamepad); // closes its joystick too
            else
                SDL_CloseJoystick(d.joystick);
        }
        devices().clear();
    }

    void openDevices()
    {
        int count = 0;
        SDL_JoystickID *ids = SDL_GetJoysticks(&count);
        std::vector<SDL_JoystickID> now(ids, ids + (ids ? count : 0));
        std::vector<SDL_JoystickID> open;
        for (const Device &d : devices())
            open.push_back(SDL_GetJoystickID(d.joystick));
        if (now == open)
        {
            SDL_free(ids);
            return;
        }
        closeDevices();
        for (int i = 0; i < count && ids; ++i)
        {
            Device d;
            if (SDL_IsGamepad(ids[i]))
            {
                d.gamepad = SDL_OpenGamepad(ids[i]);
                d.joystick = d.gamepad ? SDL_GetGamepadJoystick(d.gamepad) : nullptr;
            }
            else
            {
                d.joystick = SDL_OpenJoystick(ids[i]);
            }
            if (!d.joystick)
            {
                std::cerr << "[input] open: " << SDL_GetError() << std::endl;
                continue;
            }
            const std::string name = SDL_GetJoystickName(d.joystick) ? SDL_GetJoystickName(d.joystick) : "";
            d.name = name;
            const auto builtin = builtinProfile(d.joystick, d.gamepad);
            const Section *own = sectionOf(name);
            d.sectionName = name;
            d.builtin = builtin ? builtin->profile : Profile{};
            d.profile = overlay(d.builtin, own);
            const bool bound = own && !own->empty();
            std::cerr << "[input] " << name << ": " << (builtin ? builtin->label : "no built-in layout")
                      << (bound ? ", with input.ini" : "") << std::endl;
            if (!builtin && !bound)
                std::cerr << "[input]   unbound; run GHClassic --bind to set it up" << std::endl;
            listInFile(name, d.builtin);
            d.now = read(d);
            devices().push_back(d);
        }
        SDL_free(ids);
    }

    void readSdlDevices()
    {
        for (Device &d : devices())
            d.now = read(d);
    }
}
