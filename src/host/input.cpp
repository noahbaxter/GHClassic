// Keyboard and gamepad, read through SDL into one PS2 pad.
//
// The pad is a RedOctane guitar (see pad.cpp), so the keyboard plays one:
//   1 2 3 4 5   frets green..orange: R2 Circle Triangle Cross Square, the
//               guitar slots in GH2's config/beatmatch_controller.dta. Green
//               confirms and red backs out in menus
//   up down     strum, and menu movement
//   Enter       Start
//   Right Shift Select (star power)
//
// A gamepad maps by position (south Cross, east Circle, triggers L2/R2), so
// on it the frets land on the guitar's buttons, not a fret layout.

#include "input.h"

#include <SDL3/SDL.h>

#include <iostream>
#include <mutex>

namespace gh2
{
    namespace
    {
        bool s_open = false;
        SDL_Gamepad *s_gamepad = nullptr;

        std::mutex s_mutex;
        HostPad s_pad;

        uint8_t stickByte(Sint16 v)
        {
            return static_cast<uint8_t>((v + 32768) >> 8);
        }

        void chooseGamepad()
        {
            if (s_gamepad && SDL_GamepadConnected(s_gamepad))
                return;
            if (s_gamepad)
                SDL_CloseGamepad(s_gamepad);
            s_gamepad = nullptr;

            int count = 0;
            SDL_JoystickID *ids = SDL_GetGamepads(&count);
            if (ids && count > 0)
            {
                s_gamepad = SDL_OpenGamepad(ids[0]);
                if (s_gamepad)
                    std::cerr << "[input] gamepad: " << SDL_GetGamepadName(s_gamepad) << std::endl;
                else
                    std::cerr << "[input] gamepad open: " << SDL_GetError() << std::endl;
            }
            SDL_free(ids);
        }

        void readKeyboard(HostPad &pad)
        {
            struct Key
            {
                SDL_Scancode key;
                uint16_t button;
            };
            static constexpr Key kKeys[] = {
                {SDL_SCANCODE_UP, pad::kUp},
                {SDL_SCANCODE_DOWN, pad::kDown},
                {SDL_SCANCODE_1, pad::kR2},
                {SDL_SCANCODE_2, pad::kCircle},
                {SDL_SCANCODE_3, pad::kTriangle},
                {SDL_SCANCODE_4, pad::kCross},
                {SDL_SCANCODE_5, pad::kSquare},
                {SDL_SCANCODE_RETURN, pad::kStart},
                {SDL_SCANCODE_RSHIFT, pad::kSelect},
            };
            const bool *keys = SDL_GetKeyboardState(nullptr);
            for (const Key &k : kKeys)
                if (keys[k.key])
                    pad.pressed |= k.button;
        }

        void readGamepad(HostPad &pad)
        {
            if (!s_gamepad)
                return;
            struct Button
            {
                SDL_GamepadButton button;
                uint16_t bit;
            };
            static constexpr Button kButtons[] = {
                {SDL_GAMEPAD_BUTTON_DPAD_UP, pad::kUp},
                {SDL_GAMEPAD_BUTTON_DPAD_DOWN, pad::kDown},
                {SDL_GAMEPAD_BUTTON_DPAD_LEFT, pad::kLeft},
                {SDL_GAMEPAD_BUTTON_DPAD_RIGHT, pad::kRight},
                {SDL_GAMEPAD_BUTTON_SOUTH, pad::kCross},
                {SDL_GAMEPAD_BUTTON_EAST, pad::kCircle},
                {SDL_GAMEPAD_BUTTON_WEST, pad::kSquare},
                {SDL_GAMEPAD_BUTTON_NORTH, pad::kTriangle},
                {SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, pad::kL1},
                {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, pad::kR1},
                {SDL_GAMEPAD_BUTTON_LEFT_STICK, pad::kL3},
                {SDL_GAMEPAD_BUTTON_RIGHT_STICK, pad::kR3},
                {SDL_GAMEPAD_BUTTON_BACK, pad::kSelect},
                {SDL_GAMEPAD_BUTTON_START, pad::kStart},
            };
            for (const Button &b : kButtons)
                if (SDL_GetGamepadButton(s_gamepad, b.button))
                    pad.pressed |= b.bit;
            // Triggers run 0..32767.
            if (SDL_GetGamepadAxis(s_gamepad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > 16384)
                pad.pressed |= pad::kL2;
            if (SDL_GetGamepadAxis(s_gamepad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > 16384)
                pad.pressed |= pad::kR2;
            pad.lx = stickByte(SDL_GetGamepadAxis(s_gamepad, SDL_GAMEPAD_AXIS_LEFTX));
            pad.ly = stickByte(SDL_GetGamepadAxis(s_gamepad, SDL_GAMEPAD_AXIS_LEFTY));
            pad.rx = stickByte(SDL_GetGamepadAxis(s_gamepad, SDL_GAMEPAD_AXIS_RIGHTX));
            pad.ry = stickByte(SDL_GetGamepadAxis(s_gamepad, SDL_GAMEPAD_AXIS_RIGHTY));
        }
    }

    void openInput()
    {
        // The controller keeps playing while the window is behind the
        // terminal that launched it.
        SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
        if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD))
        {
            std::cerr << "[input] gamepad init: " << SDL_GetError() << std::endl;
            return;
        }
        s_open = true;
        chooseGamepad();
    }

    void pollInput(bool devicesChanged)
    {
        if (!s_open)
            return;
        if (devicesChanged)
            chooseGamepad();
        HostPad pad;
        readKeyboard(pad);
        readGamepad(pad);
        std::lock_guard<std::mutex> lock(s_mutex);
        s_pad = pad;
    }

    void closeInput()
    {
        if (!s_open)
            return;
        if (s_gamepad)
            SDL_CloseGamepad(s_gamepad);
        s_gamepad = nullptr;
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
