#include "host/capture.h"

#include <algorithm>
#include <cmath>

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
        case Source::kPadAccel:
            return at(now.accel, s.index) ? travel(now.accel[static_cast<size_t>(s.index)]) : 0.0f;
        }
        return 0.0f;
    }

    void play(const Profile &profile, const Snapshot &now, const bool *keys, std::array<float, kActionCount> &held)
    {
        for (int a = 0; a < kActionCount; ++a)
            for (const Source &s : profile[a])
                held[a] = std::max(held[a], amount(s, now, keys));
    }

    std::optional<Source> moved(const Snapshot &rest, const Snapshot &now)
    {
        for (size_t i = 0; i < now.buttons.size() && i < rest.buttons.size(); ++i)
            if (now.buttons[i] && !rest.buttons[i])
                return Source{Source::kButton, static_cast<int>(i)};
        for (size_t i = 0; i < now.hats.size() && i < rest.hats.size(); ++i)
            for (uint8_t bit : {SDL_HAT_UP, SDL_HAT_RIGHT, SDL_HAT_DOWN, SDL_HAT_LEFT})
                if ((now.hats[i] & bit) && !(rest.hats[i] & bit))
                    return Source{Source::kHat, static_cast<int>(i), bit};
        for (size_t i = 0; i < now.axes.size() && i < rest.axes.size(); ++i)
            if (std::fabs(now.axes[i] - rest.axes[i]) > 0.5f)
                return Source{Source::kAxis, static_cast<int>(i), 0, rest.axes[i], now.axes[i]};
        return std::nullopt;
    }

    bool atRest(const Snapshot &rest, const Snapshot &now)
    {
        if (now.buttons != rest.buttons || now.hats != rest.hats)
            return false;
        for (size_t i = 0; i < now.axes.size() && i < rest.axes.size(); ++i)
            if (std::fabs(now.axes[i] - rest.axes[i]) > 0.25f)
                return false;
        return true;
    }

    bool backAtRest(const Source &s, const Snapshot &rest, const Snapshot &now)
    {
        const auto i = static_cast<size_t>(s.index);
        switch (s.kind)
        {
        case Source::kButton:
            return i < now.buttons.size() && i < rest.buttons.size() && now.buttons[i] == rest.buttons[i];
        case Source::kHat:
            return i < now.hats.size() && i < rest.hats.size() && (now.hats[i] & s.hat) == (rest.hats[i] & s.hat);
        case Source::kAxis:
            return i < now.axes.size() && i < rest.axes.size() && std::fabs(now.axes[i] - rest.axes[i]) <= 0.25f;
        default:
            return atRest(rest, now);
        }
    }

    bool Capture::feedTilt(const Snapshot &now)
    {
        // A button or hat is a tilt switch, caught as any control is.
        if (const auto m = moved(m_rest, now); m && m->kind != Source::kAxis && !m_found)
            m_found = m;
        if (m_found && m_found->kind != Source::kAxis && m_found->kind != Source::kPadAccel)
            return backAtRest(*m_found, m_rest, now);

        // The accelerometer's first reading is its rest: it sends nothing
        // until turned on.
        if (m_rest.accel.size() != now.accel.size())
            m_rest.accel = now.accel;
        const size_t axes = std::min(now.axes.size(), m_rest.axes.size());
        const size_t count = axes + now.accel.size();
        m_peak.resize(count, 0.0f);

        // Each reading's travel from rest as a share of full: an axis's
        // whole, or one g for the accelerometer.
        constexpr float kG = 9.81f;
        const auto share = [&](size_t i, float v) { return i < axes ? v : v / kG; };
        const auto delta = [&](size_t i) {
            return i < axes ? now.axes[i] - m_rest.axes[i] : now.accel[i - axes] - m_rest.accel[i - axes];
        };
        // Caught once one that moved far enough has come most of the way
        // back, the furthest of those: another still out (a sensor the
        // player's hands keep moving) holds nothing up.
        constexpr float kMoved = 0.2f;
        size_t best = count;
        for (size_t i = 0; i < count; ++i)
        {
            if (std::fabs(delta(i)) > std::fabs(m_peak[i]))
                m_peak[i] = delta(i);
            const bool back = std::fabs(delta(i)) < 0.4f * std::fabs(m_peak[i]);
            if (back && std::fabs(share(i, m_peak[i])) >= kMoved &&
                (best == count || std::fabs(share(i, m_peak[i])) > std::fabs(share(best, m_peak[best]))))
                best = i;
        }
        if (best == count)
            return false;
        if (best < axes)
            m_found = Source{Source::kAxis, static_cast<int>(best), 0, m_rest.axes[best], m_rest.axes[best] + m_peak[best]};
        else
        {
            const size_t a = best - axes;
            m_found = Source{Source::kPadAccel, static_cast<int>(a), 0, m_rest.accel[a], m_rest.accel[a] + m_peak[best]};
        }
        return true;
    }

    bool Capture::feed(const Snapshot &now)
    {
        if (m_mode == kTilt)
            return feedTilt(now);
        if (!m_found && m_mode == kAxisOnly)
        {
            // Its axes alone: a button pressed with it (a stick's click, a
            // pad's touch sensor) would be found first every time.
            Snapshot axes = now;
            axes.buttons = m_rest.buttons;
            axes.hats = m_rest.hats;
            m_found = moved(m_rest, axes);
        }
        else if (!m_found)
            m_found = moved(m_rest, now);
        if (m_found && m_found->kind == Source::kAxis && m_found->index < static_cast<int>(now.axes.size()))
        {
            const float v = now.axes[static_cast<size_t>(m_found->index)];
            if (std::fabs(v - m_found->rest) > std::fabs(m_found->full - m_found->rest))
                m_found->full = v;
        }
        return m_found && backAtRest(*m_found, m_rest, now);
    }
}
