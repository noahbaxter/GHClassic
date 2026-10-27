#include "host/capture.h"

#include <algorithm>

namespace gh2::input
{
    Snapshot readJoystick(SDL_Joystick *joystick)
    {
        Snapshot s;
        for (int i = 0; i < SDL_GetNumJoystickButtons(joystick); ++i)
            s.buttons.push_back(SDL_GetJoystickButton(joystick, i));
        for (int i = 0; i < SDL_GetNumJoystickHats(joystick); ++i)
            s.hats.push_back(SDL_GetJoystickHat(joystick, i));
        for (int i = 0; i < SDL_GetNumJoystickAxes(joystick); ++i)
            s.axes.push_back(axisValue(SDL_GetJoystickAxis(joystick, i)));
        return s;
    }

    float amount(const Source &s, const Snapshot &now, const bool *keys)
    {
        const auto at = [](const auto &values, int i) { return i >= 0 && static_cast<size_t>(i) < values.size(); };
        const auto travel = [&](float value) { return std::clamp((value - s.rest) / (s.full - s.rest), 0.0f, 1.0f); };
        switch (s.kind)
        {
        case Source::kKey:
            return keys && keys[s.index] ? 1.0f : 0.0f;
        case Source::kButton:
            return at(now.buttons, s.index) && now.buttons[static_cast<size_t>(s.index)] ? 1.0f : 0.0f;
        case Source::kHat:
            return at(now.hats, s.index) && (now.hats[static_cast<size_t>(s.index)] & s.hat) ? 1.0f : 0.0f;
        case Source::kAxis:
            return at(now.axes, s.index) ? travel(now.axes[static_cast<size_t>(s.index)]) : 0.0f;
        case Source::kPadButton:
            return at(now.padButtons, s.index) && now.padButtons[static_cast<size_t>(s.index)] ? 1.0f : 0.0f;
        case Source::kPadAxis:
            return at(now.padAxes, s.index) ? travel(now.padAxes[static_cast<size_t>(s.index)]) : 0.0f;
        }
        return 0.0f;
    }

    void play(const Profile &profile, const Snapshot &now, const bool *keys, std::array<float, kActionCount> &held)
    {
        for (int a = 0; a < kActionCount; ++a)
            for (const Source &s : profile[a])
                held[a] = std::max(held[a], amount(s, now, keys));
    }
}
