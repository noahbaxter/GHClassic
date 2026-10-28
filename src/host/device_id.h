#pragma once

// Who a device is, and what kind of thing a player holds.

#include <compare>
#include <string>

namespace gh2::input
{
    struct DeviceId
    {
        // What reads it.
        enum Backend
        {
            kNone,
            kKeyboard,
            kSdl,      // a controller SDL opened
            kScripted, // a scenario's
        };
        Backend backend = kNone;
        // SDL's GUID, the same for every one of a model, so one fix in
        // input.ini covers both of a pair; a scenario's device's name.
        std::string model;
        // Where it is plugged in, which tells two of a model apart and finds
        // one replugged into the same port.
        std::string port;

        static DeviceId keyboard() { return {kKeyboard, "", ""}; }
        bool none() const { return backend == kNone; }
        // Two controllers SDL reads alike.
        bool sameModel(const DeviceId &other) const
        {
            return backend == kSdl && other.backend == kSdl && model == other.model;
        }
        auto operator<=>(const DeviceId &) const = default;

        // As input.ini's [seats] keeps it: "<GUID> <path>", keyboard, or
        // script <name>.
        std::string text() const
        {
            switch (backend)
            {
            case kKeyboard:
                return "keyboard";
            case kSdl:
                return model + (port.empty() ? "" : " " + port);
            case kScripted:
                return "script " + model;
            default:
                return "";
            }
        }
        static DeviceId parse(const std::string &text)
        {
            if (text.empty())
                return {};
            if (text == "keyboard")
                return keyboard();
            if (text.rfind("script ", 0) == 0)
                return {kScripted, text.substr(7), ""};
            const size_t space = text.find(' ');
            return {kSdl, text.substr(0, space), space == std::string::npos ? "" : text.substr(space + 1)};
        }
    };

    // What a score is filed by, and the icon a device is shown with.
    enum class DeviceKind
    {
        kController, // bound only in input.ini
        kGuitar,
        kGamepad,
        kKeyboard,
    };
    inline const char *kindName(DeviceKind kind)
    {
        switch (kind)
        {
        case DeviceKind::kGuitar:
            return "guitar";
        case DeviceKind::kGamepad:
            return "gamepad";
        case DeviceKind::kKeyboard:
            return "keyboard";
        default:
            return "controller";
        }
    }
}
