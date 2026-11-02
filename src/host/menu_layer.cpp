#include "host/menu_layer.h"

#include "host/input.h"
#include "host/layouts.h"
#include "host/sdl_devices.h"

#include <vector>

namespace gh2::input
{
    namespace
    {
        Source stick(SDL_GamepadAxis a, float full)
        {
            Source s;
            s.kind = Source::kPadAxis;
            s.index = a;
            s.rest = 0.0f;
            s.full = full;
            return s;
        }

        // The actions a menu reads: the frets, strum up and down as its up
        // and down, Start, and star power as Select.
        constexpr Action kMenuActions[] = {kGreen,   kRed,      kYellow, kBlue,     kOrange,
                                           kStrumUp, kStrumDown, kStart, kStarPower};

        std::array<std::string, kActionCount> shownFor(const SDL_Gamepad *gamepad, const Profile &profile)
        {
            std::array<std::string, kActionCount> shownAs;
            for (const Action a : kMenuActions)
                if (!profile[a].empty())
                    shownAs[a] = shown(gamepad, profile[a].front());
            return shownAs;
        }
    }

    std::vector<MenuKey> keyboardMenuKeys()
    {
        // Left and right are always the arrows; Esc is always there, as a
        // song's Start whatever a menu binds it to.
        std::vector<MenuKey> keys = {{SDL_SCANCODE_LEFT, pad::kLeft}, {SDL_SCANCODE_RIGHT, pad::kRight}};
        bool esc = false;
        const Profile &profile = menuLayout(true);
        for (const Action a : kMenuActions)
            for (const Source &s : profile[a])
            {
                keys.push_back({static_cast<SDL_Scancode>(s.index), kActions[a].button});
                esc = esc || s.index == SDL_SCANCODE_ESCAPE;
            }
        if (!esc)
            keys.push_back({SDL_SCANCODE_ESCAPE, 0});
        return keys;
    }

    bool hasMenuLayer(const Snapshot &now)
    {
        return !now.padButtons.empty();
    }

    uint16_t gamepadMenuButtons(const Snapshot &now, StickLatch &latch)
    {
        uint16_t bits = 0;
        const Profile &profile = menuLayout(false);
        for (const Action a : kMenuActions)
            for (const Source &s : profile[a])
                if (amount(s, now, nullptr) > kHeld)
                    bits |= kActions[a].button;
        latch.up = stickHeld(amount(stick(SDL_GAMEPAD_AXIS_LEFTY, -1.0f), now, nullptr), latch.up);
        latch.down = stickHeld(amount(stick(SDL_GAMEPAD_AXIS_LEFTY, 1.0f), now, nullptr), latch.down);
        if (latch.up)
            bits |= pad::kUp;
        if (latch.down)
            bits |= pad::kDown;
        return bits;
    }

    std::array<std::string, kActionCount> gamepadMenuShown(const SDL_Gamepad *gamepad)
    {
        return shownFor(gamepad, menuLayout(false));
    }

    std::array<std::string, kActionCount> keyboardMenuShown()
    {
        return shownFor(nullptr, menuLayout(true));
    }
}
