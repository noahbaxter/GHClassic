// Bind mode: find each action on a device by what moves away from rest.
//
// The device must be left alone while its resting state is taken. Each
// action then takes the first control that moves: a key, a button, a hat
// direction, or an axis moved over half its travel. Escape, or ten seconds
// untouched, skips an action. Whammy is the axis pushed furthest while the
// prompt is up, from its rest to the furthest point.

#include "host/bind.h"

#include "host/bindings.h"

#include <SDL3/SDL.h>

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace gh2
{
    namespace
    {
        using namespace input;
        using Clock = std::chrono::steady_clock;

        constexpr auto kSkipAfter = std::chrono::seconds(10);

        struct Snapshot
        {
            std::vector<bool> buttons;
            std::vector<uint8_t> hats;
            std::vector<float> axes;
        };

        const char *const kPrompts[kActionCount] = {
            "press green",
            "press red",
            "press yellow",
            "press blue",
            "press orange",
            "strum up",
            "strum down",
            "press start",
            "press star power (select)",
            "tilt the guitar up",
            "push the whammy all the way, then let go",
        };

        SDL_Window *s_window = nullptr;

        float axisValue(Sint16 v)
        {
            return v < 0 ? v / 32768.0f : v / 32767.0f;
        }

        Snapshot read(SDL_Joystick *joy)
        {
            Snapshot s;
            for (int i = 0; i < SDL_GetNumJoystickButtons(joy); ++i)
                s.buttons.push_back(SDL_GetJoystickButton(joy, i));
            for (int i = 0; i < SDL_GetNumJoystickHats(joy); ++i)
                s.hats.push_back(SDL_GetJoystickHat(joy, i));
            for (int i = 0; i < SDL_GetNumJoystickAxes(joy); ++i)
                s.axes.push_back(axisValue(SDL_GetJoystickAxis(joy, i)));
            return s;
        }

        // Pumps events; false when the window closed. Collects keys pressed.
        bool pump(std::vector<SDL_Scancode> *keysDown)
        {
            SDL_Event e;
            while (SDL_PollEvent(&e))
            {
                if (e.type == SDL_EVENT_QUIT || e.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED)
                    return false;
                if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat && keysDown)
                    keysDown->push_back(e.key.scancode);
            }
            return true;
        }

        void prompt(const std::string &text)
        {
            std::cout << "  " << text << " ... " << std::flush;
            SDL_SetWindowTitle(s_window, ("GH Classic bind: " + text).c_str());
        }

        // The first control away from rest, as a source.
        std::optional<Source> moved(const Snapshot &rest, const Snapshot &now)
        {
            for (size_t i = 0; i < now.buttons.size(); ++i)
                if (now.buttons[i] && !rest.buttons[i])
                    return Source{Source::kButton, static_cast<int>(i)};
            for (size_t i = 0; i < now.hats.size(); ++i)
                for (uint8_t bit : {SDL_HAT_UP, SDL_HAT_RIGHT, SDL_HAT_DOWN, SDL_HAT_LEFT})
                    if ((now.hats[i] & bit) && !(rest.hats[i] & bit))
                        return Source{Source::kHat, static_cast<int>(i), bit};
            for (size_t i = 0; i < now.axes.size(); ++i)
                if (std::fabs(now.axes[i] - rest.axes[i]) > 0.5f)
                    return Source{Source::kAxis, static_cast<int>(i), 0, rest.axes[i], now.axes[i]};
            return std::nullopt;
        }

        bool atRest(const Snapshot &rest, const Snapshot &now)
        {
            if (now.buttons != rest.buttons || now.hats != rest.hats)
                return false;
            for (size_t i = 0; i < now.axes.size(); ++i)
                if (std::fabs(now.axes[i] - rest.axes[i]) > 0.25f)
                    return false;
            return true;
        }

        // A key for the keyboard, a control for a joystick; nothing on skip.
        // An axis keeps the furthest point it reached before letting go.
        std::optional<Source> capture(SDL_Joystick *joy, const Snapshot &rest, bool whammy, bool &quit)
        {
            const auto start = Clock::now();
            std::optional<Source> found;
            while (Clock::now() - start < kSkipAfter)
            {
                std::vector<SDL_Scancode> keys;
                if (!pump(&keys))
                {
                    quit = true;
                    return std::nullopt;
                }
                for (SDL_Scancode k : keys)
                {
                    if (k == SDL_SCANCODE_ESCAPE)
                        return std::nullopt;
                    if (!joy)
                        return Source{Source::kKey, k};
                }
                if (joy)
                {
                    const Snapshot now = read(joy);
                    if (!found)
                    {
                        const auto m = moved(rest, now);
                        if (m && (!whammy || m->kind == Source::kAxis))
                            found = m;
                    }
                    if (found && found->kind == Source::kAxis)
                    {
                        const float v = now.axes[found->index];
                        if (std::fabs(v - found->rest) > std::fabs(found->full - found->rest))
                            found->full = v;
                    }
                    if (found && atRest(rest, now))
                        return found;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            return found;
        }

        int list()
        {
            int count = 0;
            SDL_JoystickID *ids = SDL_GetJoysticks(&count);
            std::cout << "devices:\n  keyboard\n";
            for (int i = 0; i < count && ids; ++i)
                std::cout << "  " << i << "  " << (SDL_GetJoystickNameForID(ids[i]) ? SDL_GetJoystickNameForID(ids[i]) : "?")
                          << (SDL_IsGamepad(ids[i]) ? "  (gamepad)" : "") << "\n";
            SDL_free(ids);
            std::cout << "bind one with: GHClassic --bind keyboard | GHClassic --bind <n>" << std::endl;
            return 0;
        }
    }

    int runBind(int argc, char *argv[])
    {
        SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
        if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD))
        {
            std::cerr << "[bind] init: " << SDL_GetError() << std::endl;
            return 1;
        }
        // SDL reports controllers present at start as added events.
        pump(nullptr);

        const std::string target = argc > 2 ? argv[2] : "";
        if (target.empty())
        {
            const int result = list();
            SDL_Quit();
            return result;
        }

        SDL_Joystick *joy = nullptr;
        std::string section = keyboardSection();
        if (target != "keyboard")
        {
            int count = 0;
            SDL_JoystickID *ids = SDL_GetJoysticks(&count);
            const int n = std::atoi(target.c_str());
            if (ids && n >= 0 && n < count && target.find_first_not_of("0123456789") == std::string::npos)
                joy = SDL_OpenJoystick(ids[n]);
            SDL_free(ids);
            if (!joy)
            {
                std::cerr << "[bind] no device " << target << std::endl;
                list();
                SDL_Quit();
                return 2;
            }
            section = deviceSection(SDL_GetJoystickName(joy) ? SDL_GetJoystickName(joy) : "");
        }

        s_window = SDL_CreateWindow("GH Classic bind", 480, 120, 0);
        std::cout << "binding [" << section << "]. Escape or 10 seconds skips an action.\n";
        Snapshot rest;
        if (joy)
        {
            std::cout << "  leave the controller alone ... " << std::flush;
            const auto until = Clock::now() + std::chrono::milliseconds(1000);
            while (Clock::now() < until)
            {
                pump(nullptr);
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            rest = read(joy);
            std::cout << "ok\n";
        }

        std::map<Action, std::vector<Source>> bound;
        bool quit = false;
        for (int a = 0; a < kActionCount && !quit; ++a)
        {
            prompt(kPrompts[a]);
            const auto source = capture(joy, rest, a == kWhammy, quit);
            if (source)
            {
                bound[static_cast<Action>(a)] = {*source};
                std::cout << formatSource(*source) << "\n";
            }
            else
            {
                std::cout << (quit ? "stopped\n" : "skipped\n");
            }
            // Let go of it before the next prompt.
            const auto until = Clock::now() + std::chrono::milliseconds(300);
            while (!quit && Clock::now() < until)
                quit = !pump(nullptr);
        }

        int result = 0;
        if (quit)
        {
            std::cout << "nothing written" << std::endl;
        }
        else
        {
            const std::string path = bindingFilePath();
            if (writeSection(path, section, bound))
                std::cout << "wrote [" << section << "] to " << path << std::endl;
            else
            {
                std::cerr << "[bind] could not write " << path << std::endl;
                result = 1;
            }
        }
        if (joy)
            SDL_CloseJoystick(joy);
        SDL_DestroyWindow(s_window);
        SDL_Quit();
        return result;
    }
}
