// Bind mode: find each action on a device by what moves away from rest.
//
// The device must be left alone while its resting state is taken. Each
// action then takes the first control that moves: a key, a button, a hat
// direction, or an axis moved over half its travel (host/capture.h). Escape,
// or ten seconds untouched, skips an action, which keeps what input.ini had
// for it.

#include "host/bind.h"

#include "host/bindings.h"
#include "host/capture.h"

#include <SDL3/SDL.h>

#include <chrono>
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

        // A key for the keyboard, a control for a joystick; nothing on skip.
        std::optional<Source> capture(SDL_Joystick *joy, const Snapshot &rest, bool whammy, bool &quit)
        {
            const auto start = Clock::now();
            Capture caught(rest, whammy ? Capture::kAxisOnly : Capture::kAny);
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
                if (joy && caught.feed(readJoystick(joy)))
                    return caught.found();
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            return caught.found();
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
        std::string name;
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
            name = SDL_GetJoystickName(joy) ? SDL_GetJoystickName(joy) : "";
            section = deviceSection(name, joystickGuid(joy));
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
            rest = readJoystick(joy);
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
        if (quit || bound.empty())
        {
            std::cout << "nothing written" << std::endl;
        }
        else
        {
            // A skipped action keeps what the device's section already had,
            // an older file's by name alone included.
            const std::string path = bindingFilePath();
            const BindingFile file = loadBindingFile(path);
            const auto *had = joy ? deviceBindings(file, name, joystickGuid(joy)) : nullptr;
            if (!joy && file.sections.count(section))
                had = &file.sections.at(section);
            std::map<Action, std::vector<Source>> merged = had ? *had : std::map<Action, std::vector<Source>>{};
            for (auto &[action, sources] : bound)
                merged[action] = std::move(sources);
            if (writeSection(path, section, merged))
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
