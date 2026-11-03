#pragma once

// What a gamepad or the keyboard presses in a menu, apart from what its
// layout plays in a song. A menu reads its own controls the way a console does
// (A picks, B backs out, the D-pad or left stick moves, Start is Start; Y, X
// and LB are a menu's yellow, blue and orange, Back its Select), so frets mapped to A or B
// never move a menu and a menu never reads a fret. input.ini's
// [menu keyboard] and [menu gamepad] rebind them (layouts.h).
// A guitar has no such controls and keeps its layout's. The host thread's
// own.

#include "host/bindings.h"
#include "host/capture.h"
#include "host/input.h"

#include <SDL3/SDL.h>

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace gh2::input
{
    // The left stick as a menu's up or down: it presses past 75% and lets go
    // only below 35%, so the rebound of a stick let go of (half way back out
    // the way it came) is not a second push.
    inline bool stickHeld(float amount, bool was)
    {
        return amount > (was ? 0.35f : 0.75f);
    }
    struct StickLatch
    {
        bool up = false, down = false;
    };

    // Up and down held in a menu (a strum bar too) step again after a moment
    // and then steadily, sooner than GH2's own repeat, which takes a couple of
    // seconds: the button lets go for a beat each step, so the game sees a
    // fresh press.
    constexpr uint64_t kNavDelayMs = 600, kNavStepMs = 90, kNavGapMs = 35;
    struct NavRepeat
    {
        uint64_t since[2] = {0, 0}; // when up, then down, was first held; 0 for not
    };
    inline uint16_t navRepeat(uint16_t held, uint64_t nowMs, NavRepeat &state)
    {
        uint16_t out = held;
        const uint16_t dirs[2] = {pad::kUp, pad::kDown};
        for (int i = 0; i < 2; ++i)
        {
            if (!(held & dirs[i]))
            {
                state.since[i] = 0;
                continue;
            }
            if (state.since[i] == 0)
                state.since[i] = nowMs;
            const uint64_t heldMs = nowMs - state.since[i];
            if (heldMs >= kNavDelayMs && (heldMs - kNavDelayMs) % kNavStepMs < kNavGapMs)
                out = static_cast<uint16_t>(out & ~dirs[i]);
        }
        return out;
    }

    // The keyboard's menu keys (layouts.h's menuLayout), each with the
    // guitar's libpad button it presses; the arrows' left and right, and Esc,
    // which in a song is Start whatever a menu binds it to (bit 0 when a
    // menu binds it to nothing).
    struct MenuKey
    {
        SDL_Scancode key;
        uint16_t bit;
    };
    std::vector<MenuKey> keyboardMenuKeys();

    // A gamepad's menu controls as a guitar's libpad buttons: green picks,
    // red backs out, strum up and down move, Start. Empty when SDL gave the
    // gamepad no layout (`now` has no gamepad buttons): its own then.
    bool hasMenuLayer(const Snapshot &now);
    uint16_t gamepadMenuButtons(const Snapshot &now, StickLatch &stick);

    // The controls a menu reads, as the help bar's glyphs name them (by
    // action, "" for those a menu does not read).
    std::array<std::string, kActionCount> gamepadMenuShown(const SDL_Gamepad *gamepad);
    std::array<std::string, kActionCount> keyboardMenuShown();
}
