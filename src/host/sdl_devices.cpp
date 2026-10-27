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

        bool sdl(const Device &d)
        {
            return d.id.backend == DeviceId::kSdl;
        }
    }

    void closeDevices()
    {
        for (Device &d : devices())
        {
            if (!sdl(d))
                continue;
            if (d.gamepad)
                SDL_CloseGamepad(d.gamepad); // closes its joystick too
            else
                SDL_CloseJoystick(d.joystick);
        }
        std::erase_if(devices(), sdl);
    }

    void openDevices()
    {
        int count = 0;
        SDL_JoystickID *ids = SDL_GetJoysticks(&count);
        std::vector<SDL_JoystickID> now(ids, ids + (ids ? count : 0));
        std::vector<SDL_JoystickID> open;
        for (const Device &d : devices())
            if (sdl(d))
                open.push_back(SDL_GetJoystickID(d.joystick));
        if (now == open)
        {
            SDL_free(ids);
            return;
        }
        closeDevices();
        std::vector<Device> opened;
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
            // Its GUID and path: the same controller in the same port after
            // a replug, where SDL's instance ID is new.
            const std::string guid = joystickGuid(d.joystick);
            const char *path = SDL_GetJoystickPath(d.joystick);
            d.id = {DeviceId::kSdl, guid, path ? path : ""};
            // Two of the same controller share a name; the log tells them
            // apart by where they are plugged in.
            if (path)
                d.name += std::string(" (") + path + ")";
            const DeviceInfo info = deviceInfo(d.joystick, d.gamepad);
            const auto builtin = builtinProfile(info);
            if (builtin)
                d.kind = builtin->gamepad ? DeviceKind::kGamepad : DeviceKind::kGuitar;
            d.label = builtin && !builtin->gamepad ? builtin->label : name;
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
            d.now = d.rest = read(d);
            d.openedMs = SDL_GetTicks();
            opened.push_back(d);
        }
        SDL_free(ids);
        // Ahead of a scenario's, as SDL lists them.
        devices().insert(devices().begin(), opened.begin(), opened.end());
    }

    void readSdlDevices()
    {
        for (Device &d : devices())
            if (sdl(d))
                d.now = read(d);
    }

    void enableAccel(Device &d)
    {
        if (d.gamepad && SDL_GamepadHasSensor(d.gamepad, SDL_SENSOR_ACCEL))
            SDL_SetGamepadSensorEnabled(d.gamepad, SDL_SENSOR_ACCEL, true);
    }

    std::string shown(const SDL_Gamepad *gamepad, const Source &s)
    {
        const bool playStation =
            gamepad && SDL_GetGamepadButtonLabel(const_cast<SDL_Gamepad *>(gamepad), SDL_GAMEPAD_BUTTON_SOUTH) ==
                           SDL_GAMEPAD_BUTTON_LABEL_CROSS;
        const auto way = [&](const char *stick, bool vertical) {
            const bool more = s.full > s.rest;
            return std::string(stick) + (vertical ? (more ? " Down" : " Up") : (more ? " Right" : " Left"));
        };
        switch (s.kind)
        {
        case Source::kKey:
            return formatSource(s).substr(4);
        case Source::kButton:
            return "Button " + std::to_string(s.index + 1);
        case Source::kHat:
            switch (s.hat)
            {
            case SDL_HAT_UP:
                return "D-pad Up";
            case SDL_HAT_DOWN:
                return "D-pad Down";
            case SDL_HAT_LEFT:
                return "D-pad Left";
            default:
                return "D-pad Right";
            }
        case Source::kAxis:
            return "Axis " + std::to_string(s.index + 1) + (s.full > s.rest ? " +" : " -");
        case Source::kPadButton:
            switch (static_cast<SDL_GamepadButton>(s.index))
            {
            case SDL_GAMEPAD_BUTTON_SOUTH:
            case SDL_GAMEPAD_BUTTON_EAST:
            case SDL_GAMEPAD_BUTTON_WEST:
            case SDL_GAMEPAD_BUTTON_NORTH:
                switch (gamepad ? SDL_GetGamepadButtonLabel(const_cast<SDL_Gamepad *>(gamepad),
                                                            static_cast<SDL_GamepadButton>(s.index))
                                : SDL_GAMEPAD_BUTTON_LABEL_UNKNOWN)
                {
                case SDL_GAMEPAD_BUTTON_LABEL_A:
                    return "A";
                case SDL_GAMEPAD_BUTTON_LABEL_B:
                    return "B";
                case SDL_GAMEPAD_BUTTON_LABEL_X:
                    return "X";
                case SDL_GAMEPAD_BUTTON_LABEL_Y:
                    return "Y";
                case SDL_GAMEPAD_BUTTON_LABEL_CROSS:
                    return "Cross";
                case SDL_GAMEPAD_BUTTON_LABEL_CIRCLE:
                    return "Circle";
                case SDL_GAMEPAD_BUTTON_LABEL_SQUARE:
                    return "Square";
                case SDL_GAMEPAD_BUTTON_LABEL_TRIANGLE:
                    return "Triangle";
                default:
                    break;
                }
                {
                    const char *const kFaces[] = {"A", "B", "X", "Y"};
                    return kFaces[s.index];
                }
            case SDL_GAMEPAD_BUTTON_BACK:
                return playStation ? "Select" : "Back";
            case SDL_GAMEPAD_BUTTON_GUIDE:
                return "Guide";
            case SDL_GAMEPAD_BUTTON_START:
                return "Start";
            case SDL_GAMEPAD_BUTTON_LEFT_STICK:
                return playStation ? "L3" : "L Stick Press";
            case SDL_GAMEPAD_BUTTON_RIGHT_STICK:
                return playStation ? "R3" : "R Stick Press";
            case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER:
                return playStation ? "L1" : "LB";
            case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER:
                return playStation ? "R1" : "RB";
            case SDL_GAMEPAD_BUTTON_DPAD_UP:
                return "D-pad Up";
            case SDL_GAMEPAD_BUTTON_DPAD_DOWN:
                return "D-pad Down";
            case SDL_GAMEPAD_BUTTON_DPAD_LEFT:
                return "D-pad Left";
            case SDL_GAMEPAD_BUTTON_DPAD_RIGHT:
                return "D-pad Right";
            case SDL_GAMEPAD_BUTTON_TOUCHPAD:
                return "Touchpad";
            default:
                return "Extra " + std::to_string(s.index);
            }
        case Source::kPadAxis:
            switch (static_cast<SDL_GamepadAxis>(s.index))
            {
            case SDL_GAMEPAD_AXIS_LEFT_TRIGGER:
                return playStation ? "L2" : "LT";
            case SDL_GAMEPAD_AXIS_RIGHT_TRIGGER:
                return playStation ? "R2" : "RT";
            case SDL_GAMEPAD_AXIS_LEFTX:
                return way("L Stick", false);
            case SDL_GAMEPAD_AXIS_LEFTY:
                return way("L Stick", true);
            case SDL_GAMEPAD_AXIS_RIGHTX:
                return way("R Stick", false);
            default:
                return way("R Stick", true);
            }
        case Source::kPadAccel:
            return "Tilt Sensor";
        }
        return "";
    }

    std::string joined(const SDL_Gamepad *gamepad, const std::vector<Source> &sources)
    {
        std::string text;
        for (const Source &s : sources)
            text += (text.empty() ? "" : " / ") + shown(gamepad, s);
        return text;
    }
}
