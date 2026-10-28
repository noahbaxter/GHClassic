#pragma once

// Catching the control a player presses for an action: the first that moves
// away from where the device rested, kept until it is back at rest.
// An axis keeps the furthest point it reached, so whammy and tilt come out
// with their rest and full the right way round.

#include "host/bindings.h"

#include <SDL3/SDL.h>

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

namespace gh2::input
{
    // A device's raw controls at one moment: a joystick's, the same as SDL's
    // gamepad layout reads them when it has one, and its accelerometer in
    // m/s^2 when that is turned on. Every source reads off one.
    struct Snapshot
    {
        std::vector<bool> buttons;
        std::vector<uint8_t> hats;
        std::vector<float> axes;
        std::vector<bool> padButtons; // by SDL_GamepadButton
        std::vector<float> padAxes;   // by SDL_GamepadAxis
        std::vector<float> accel;
    };

    // A joystick's own controls, without its gamepad layout.
    Snapshot readJoystick(SDL_Joystick *joystick);

    // How far a source is pressed, 0 to 1. Keys read the same on every
    // device, so a controller's section can bind keys too; none without
    // `keys`.
    float amount(const Source &s, const Snapshot &now, const bool *keys);
    // Each action's amount on `profile`: the most of its sources.
    void play(const Profile &profile, const Snapshot &now, const bool *keys, std::array<float, kActionCount> &held);

    // The first control away from rest, as a source: a button or hat
    // direction gone down, or an axis moved over half its travel.
    std::optional<Source> moved(const Snapshot &rest, const Snapshot &now);
    bool atRest(const Snapshot &rest, const Snapshot &now);
    // That one control back where it rested, whatever the rest are doing:
    // a pad's touch sensors and gyro are never all still.
    bool backAtRest(const Source &s, const Snapshot &rest, const Snapshot &now);

    // One control being caught, fed a snapshot each poll.
    class Capture
    {
    public:
        enum Mode
        {
            kAny,
            kAxisOnly, // whammy, which a button cannot be
            // Tilt, which is often a sensor that moves only part of its
            // travel: a button still counts, else whichever axis or
            // accelerometer axis moved furthest, once it comes back.
            kTilt,
        };
        Capture(Snapshot rest, Mode mode) : m_rest(std::move(rest)), m_mode(mode) {}

        // True once a control moved and is back at rest.
        bool feed(const Snapshot &now);
        const std::optional<Source> &found() const { return m_found; }

    private:
        bool feedTilt(const Snapshot &now);

        Snapshot m_rest;
        Mode m_mode;
        std::optional<Source> m_found;
        // Tilt: each axis's, then each accelerometer axis's, furthest
        // reading from rest.
        std::vector<float> m_peak;
    };
}
