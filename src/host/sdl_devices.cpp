#include "host/sdl_devices.h"

#include "host/layouts.h"
#include "host/profiles.h"

#include <algorithm>
#include <cstdio>
#include <iostream>

namespace gh2::input
{
    namespace
    {
        const char *typeName(SDL_JoystickType type)
        {
            switch (type)
            {
            case SDL_JOYSTICK_TYPE_GAMEPAD:
                return "SDL type gamepad";
            case SDL_JOYSTICK_TYPE_GUITAR:
                return "SDL type guitar";
            case SDL_JOYSTICK_TYPE_DRUM_KIT:
                return "SDL type drum kit";
            default:
                return "SDL type other";
            }
        }

        // Its raw controls, its gamepad layout's, and its accelerometer
        // while that is on.
        Snapshot read(const Device &d)
        {
            Snapshot s = readJoystick(d.joystick);
            if (!d.gamepad)
                return s;
            for (int b = 0; b < SDL_GAMEPAD_BUTTON_COUNT; ++b)
                s.padButtons.push_back(SDL_GetGamepadButton(d.gamepad, static_cast<SDL_GamepadButton>(b)));
            for (int a = 0; a < SDL_GAMEPAD_AXIS_COUNT; ++a)
                s.padAxes.push_back(axisValue(SDL_GetGamepadAxis(d.gamepad, static_cast<SDL_GamepadAxis>(a))));
            float accel[3] = {};
            if (SDL_GamepadSensorEnabled(d.gamepad, SDL_SENSOR_ACCEL) &&
                SDL_GetGamepadSensorData(d.gamepad, SDL_SENSOR_ACCEL, accel, 3))
                s.accel = {accel[0], accel[1], accel[2]};
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
            const std::string guid = joystickGuid(d.joystick);
            const char *path = SDL_GetJoystickPath(d.joystick);
            // Two of the same controller share a name; the log tells them
            // apart by where they are plugged in.
            if (path)
                d.name += std::string(" (") + path + ")";
            const DeviceInfo info = deviceInfo(d.joystick, d.gamepad);
            const auto builtin = builtinProfile(info);
            const Section *own = sectionOf(name, guid);
            d.sectionName = name;
            d.sectionGuid = guid;
            d.builtin = builtin ? builtin->profile : Profile{};
            d.profile = overlay(d.builtin, own);
            const bool bound = own && !own->empty();
            char vidPid[16];
            std::snprintf(vidPid, sizeof vidPid, "%04x:%04x", info.vendor, info.product);
            std::cerr << "[input] " << name << ": " << (builtin ? builtin->label : "no built-in layout")
                      << (bound ? ", with input.ini" : "") << std::endl;
            std::cerr << "[input]   " << vidPid << ", GUID " << guid << ", " << typeName(info.type)
                      << (d.gamepad ? std::string(", ") + SDL_GetGamepadStringForType(info.gamepadType) + " gamepad"
                                    : std::string(", no gamepad layout"))
                      << std::endl;
            for (int a = 0; a < kActionCount; ++a)
            {
                std::string sources;
                for (const Source &s : d.profile[a])
                    sources += (sources.empty() ? "" : ", ") + formatSource(s);
                std::cerr << "[input]   " << kActions[a].name << " = " << (sources.empty() ? "none" : sources)
                          << std::endl;
            }
            if (!builtin && !bound)
                std::cerr << "[input]   unbound; run GHClassic --bind to set it up" << std::endl;
            if (!own)
                listInFile(name, guid, d.builtin);
            // One read off the accelerometer needs SDL to send it.
            const bool accel = std::any_of(d.profile.begin(), d.profile.end(), [](const auto &sources) {
                return std::any_of(sources.begin(), sources.end(),
                                   [](const Source &s) { return s.kind == Source::kPadAccel; });
            });
            if (accel && d.gamepad && !SDL_SetGamepadSensorEnabled(d.gamepad, SDL_SENSOR_ACCEL, true))
                std::cerr << "[input]   no accelerometer: " << SDL_GetError() << std::endl;
            d.now = read(d);
            d.openedMs = SDL_GetTicks();
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
