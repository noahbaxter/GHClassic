// The keyboard and every connected controller, read through SDL into one PS2
// guitar.
//
// Each device plays through a profile of guitar actions (bindings.h): the
// built-in one for what it is (profiles.cpp) with its section of input.ini
// laid over it. All devices play at once; an action is held when any
// device holds it.
//
// The actions land on the RedOctane guitar's buttons (see pad.cpp for why the
// pad is one): frets R2 Circle Triangle Cross Square, the guitar slots in
// config/beatmatch_controller.dta; strum on the D-pad; star power on Select
// (force_mercury); tilt on L2, the mercury_switch GuitarController defaults
// to when the guitar config names none (kPad_L2 is 0, 0x2364d8); whammy on
// the left stick's Y pushed up, the half GetWhammyBar reads (0x236718).

#include "input.h"

#include "host/bindings.h"
#include "host/profiles.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <iostream>
#include <mutex>
#include <vector>

namespace gh2
{
    namespace
    {
        using namespace input;

        struct Device
        {
            SDL_Joystick *joystick = nullptr;
            SDL_Gamepad *gamepad = nullptr; // when SDL knows its layout
            Profile profile;
        };

        bool s_open = false;
        BindingFile s_file;
        Profile s_keyboard;
        std::vector<Device> s_devices;

        std::mutex s_mutex;
        HostPad s_pad;

        constexpr uint16_t kActionButtons[kActionCount] = {
            pad::kR2, pad::kCircle, pad::kTriangle, pad::kCross, pad::kSquare, pad::kUp,
            pad::kDown, pad::kStart, pad::kSelect, pad::kL2, 0,
        };

        const std::map<Action, std::vector<Source>> *section(const std::string &name)
        {
            const auto found = s_file.sections.find(name);
            return found == s_file.sections.end() ? nullptr : &found->second;
        }

        void closeDevices()
        {
            for (Device &d : s_devices)
            {
                if (d.gamepad)
                    SDL_CloseGamepad(d.gamepad); // closes its joystick too
                else
                    SDL_CloseJoystick(d.joystick);
            }
            s_devices.clear();
        }

        // SDL reports every joystick already plugged in as added, so the first
        // pump would reopen the ones just opened; only a changed set reopens.
        void openDevices()
        {
            int count = 0;
            SDL_JoystickID *ids = SDL_GetJoysticks(&count);
            std::vector<SDL_JoystickID> now(ids, ids + (ids ? count : 0));
            std::vector<SDL_JoystickID> open;
            for (const Device &d : s_devices)
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
                const auto builtin = builtinProfile(d.joystick, d.gamepad);
                const auto *own = section(deviceSection(name));
                d.profile = overlay(builtin ? builtin->profile : Profile{}, own);
                std::cerr << "[input] " << name << ": " << (builtin ? builtin->label : "no built-in layout")
                          << (own ? ", with input.ini" : "") << std::endl;
                if (!builtin && !own)
                    std::cerr << "[input]   unbound; run GHClassic --bind to set it up" << std::endl;
                s_devices.push_back(d);
            }
            SDL_free(ids);
        }

        float travel(float value, const Source &s)
        {
            return std::clamp((value - s.rest) / (s.full - s.rest), 0.0f, 1.0f);
        }

        // How far a source is pressed, 0 to 1. Keys read the same on every
        // device, so a controller's section can bind keys too.
        float amount(const Source &s, const Device *d, const bool *keys)
        {
            switch (s.kind)
            {
            case Source::kKey:
                return keys[s.index] ? 1.0f : 0.0f;
            case Source::kButton:
                return d && s.index < SDL_GetNumJoystickButtons(d->joystick) &&
                               SDL_GetJoystickButton(d->joystick, s.index)
                           ? 1.0f
                           : 0.0f;
            case Source::kHat:
                return d && s.index < SDL_GetNumJoystickHats(d->joystick) &&
                               (SDL_GetJoystickHat(d->joystick, s.index) & s.hat) != 0
                           ? 1.0f
                           : 0.0f;
            case Source::kAxis:
                if (!d || s.index >= SDL_GetNumJoystickAxes(d->joystick))
                    return 0.0f;
                return travel(axisValue(SDL_GetJoystickAxis(d->joystick, s.index)), s);
            case Source::kPadButton:
                return d && d->gamepad && SDL_GetGamepadButton(d->gamepad, static_cast<SDL_GamepadButton>(s.index))
                           ? 1.0f
                           : 0.0f;
            case Source::kPadAxis:
                if (!d || !d->gamepad)
                    return 0.0f;
                return travel(axisValue(SDL_GetGamepadAxis(d->gamepad, static_cast<SDL_GamepadAxis>(s.index))), s);
            }
            return 0.0f;
        }

        void play(const Profile &profile, const Device *d, const bool *keys, float (&held)[kActionCount])
        {
            for (int a = 0; a < kActionCount; ++a)
                for (const Source &s : profile[a])
                    held[a] = std::max(held[a], amount(s, d, keys));
        }
    }

    void openInput()
    {
        // The controller keeps playing while the window is behind the
        // terminal that launched it.
        SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
        if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD))
        {
            std::cerr << "[input] init: " << SDL_GetError() << std::endl;
            return;
        }
        const std::string path = bindingFilePath();
        s_file = loadBindingFile(path);
        s_keyboard = overlay(keyboardProfile(), section(keyboardSection()));
        std::cerr << "[input] bindings: " << path << (s_file.sections.empty() ? " (none yet)" : "") << std::endl;
        s_open = true;
        openDevices();
    }

    void pollInput(bool devicesChanged)
    {
        if (!s_open)
            return;
        if (devicesChanged)
            openDevices();
        const bool *keys = SDL_GetKeyboardState(nullptr);
        float held[kActionCount] = {};
        play(s_keyboard, nullptr, keys, held);
        for (const Device &d : s_devices)
            play(d.profile, &d, keys, held);

        HostPad pad;
        for (int a = 0; a < kActionCount; ++a)
            if (held[a] > 0.5f)
                pad.pressed |= kActionButtons[a];
        pad.ly = static_cast<uint8_t>(std::lround(128.0f * (1.0f - held[kWhammy])));
        std::lock_guard<std::mutex> lock(s_mutex);
        s_pad = pad;
    }

    void closeInput()
    {
        if (!s_open)
            return;
        closeDevices();
        SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
        s_open = false;
        std::lock_guard<std::mutex> lock(s_mutex);
        s_pad = HostPad{};
    }

    HostPad hostPad()
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        return s_pad;
    }
}
