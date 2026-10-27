#include "host/device.h"

#include <cmath>
#include <iostream>

namespace gh2::input
{
    namespace
    {
        std::vector<Device> s_devices;

        // Whether `bound` is the control `moved` names, an axis pushed the
        // same way.
        bool sameControl(const Source &bound, const Source &moved)
        {
            if (bound.kind != moved.kind || bound.index != moved.index)
                return false;
            if (bound.kind == Source::kHat)
                return bound.hat == moved.hat;
            if (bound.kind == Source::kAxis || bound.kind == Source::kPadAxis)
                return (bound.full > bound.rest) == (moved.full > moved.rest);
            return true;
        }
    }

    std::vector<Device> &devices()
    {
        return s_devices;
    }

    void nameControls(Device &d)
    {
        // A gamepad's controls by its layout, as it binds them.
        const bool pad = !d.now.padButtons.empty();
        std::vector<Source> moved;
        if (pad)
        {
            for (size_t b = 0; b < d.now.padButtons.size(); ++b)
                if (d.now.padButtons[b])
                    moved.push_back({Source::kPadButton, static_cast<int>(b)});
        }
        else
        {
            for (size_t b = 0; b < d.now.buttons.size(); ++b)
                if (d.now.buttons[b])
                    moved.push_back({Source::kButton, static_cast<int>(b)});
            for (size_t h = 0; h < d.now.hats.size(); ++h)
                for (uint8_t bit : {SDL_HAT_UP, SDL_HAT_RIGHT, SDL_HAT_DOWN, SDL_HAT_LEFT})
                    if (d.now.hats[h] & bit)
                        moved.push_back({Source::kHat, static_cast<int>(h), bit});
        }
        const std::vector<float> &now = pad ? d.now.padAxes : d.now.axes;
        // Its first report can come a few polls after it opens.
        if (d.logRest.size() != now.size() || SDL_GetTicks() - d.openedMs < 500u)
            d.logRest = now;
        const Source::Kind axisKind = pad ? Source::kPadAxis : Source::kAxis;
        for (size_t a = 0; a < now.size(); ++a)
            if (std::fabs(now[a] - d.logRest[a]) > 0.5f)
                moved.push_back({axisKind, static_cast<int>(a), 0, d.logRest[a], now[a] > d.logRest[a] ? 1.0f : -1.0f});
        for (const Source &m : moved)
        {
            // An axis is named by which way it went, not how far.
            Source key = m;
            key.rest = 0.0f;
            if (!d.named.insert(formatSource(key)).second)
                continue;
            std::string plays;
            for (int a = 0; a < kActionCount; ++a)
                for (const Source &s : d.profile[a])
                    if (sameControl(s, m) && plays.find(kActions[a].name) == std::string::npos)
                        plays += (plays.empty() ? "" : ", ") + std::string(kActions[a].name);
            std::cerr << "[input] " << d.name << ": " << formatSource(m) << " -> " << (plays.empty() ? "nothing" : plays)
                      << std::endl;
        }
    }
}
