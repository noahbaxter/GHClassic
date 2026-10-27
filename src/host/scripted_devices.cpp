#include "scripted_devices.h"

#include "host/bindings.h"
#include "host/device.h"
#include "host/input.h"
#include "host/layouts.h"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace gh2::input
{
    namespace
    {
        // A scenario's devices: name, a gamepad when its name says pad, and
        // the raw controls it holds: each action's button in its layout's
        // order, and whammy's axis.
        struct Scripted
        {
            std::string name;
            DeviceKind kind;
            Snapshot raw;
        };
        constexpr size_t kScriptedButtons = 16;

        Snapshot scriptedRest()
        {
            Snapshot s;
            s.buttons.assign(kScriptedButtons, false);
            s.axes.assign(1, 0.0f);
            return s;
        }

        // A scenario's device plays as its buttons and axis are laid out.
        Profile scriptedLayout()
        {
            Profile profile;
            for (int a = 0; a < kActionCount; ++a)
                if (kActions[a].button)
                    profile[a] = {{Source::kButton, a}};
            profile[kWhammy] = {{Source::kAxis, 0, 0, 0.0f, 1.0f}};
            return profile;
        }

        // A scenario's controllers attached to SDL: what each is and the
        // controls it holds, under s_mutex; and each attached, the host
        // thread's, by name, which SDL's description points into.
        struct Virtual
        {
            std::string name;
            uint16_t vendor = 0, product = 0;
            int type = SDL_JOYSTICK_TYPE_UNKNOWN;
            bool gamepad = false;
            std::vector<std::string> controls;
        };
        struct Attached
        {
            std::string name;
            SDL_JoystickID id = 0;
            SDL_Joystick *joystick = nullptr;
        };
        std::map<std::string, Attached> s_attached;

        std::mutex s_mutex;
        std::vector<Scripted> s_scripted;
        std::vector<Virtual> s_virtuals;
        // Its keys, down until let go.
        std::array<bool, SDL_SCANCODE_COUNT> s_keys{};
        bool s_keysHeld = false;

        // Its controls as `controls` hold them, the rest at rest.
        void hold(SDL_Joystick *joystick, const Virtual &v)
        {
            std::vector<bool> buttons(static_cast<size_t>(std::max(0, SDL_GetNumJoystickButtons(joystick))));
            std::vector<Sint16> axes(static_cast<size_t>(std::max(0, SDL_GetNumJoystickAxes(joystick))));
            std::vector<uint8_t> hats(static_cast<size_t>(std::max(0, SDL_GetNumJoystickHats(joystick))));
            const auto set = [](auto &values, int i, auto value) {
                if (i < 0 || static_cast<size_t>(i) >= values.size())
                    return false;
                values[static_cast<size_t>(i)] = value;
                return true;
            };
            for (const std::string &c : v.controls)
            {
                bool done = false;
                const char way = c.empty() ? 0 : c.back();
                if (way == '+' || way == '-')
                {
                    const std::string axis = c.substr(0, c.size() - 1);
                    const int at = axis.rfind("axis", 0) == 0 ? std::atoi(axis.c_str() + 4)
                                   : v.gamepad           ? SDL_GetGamepadAxisFromString(axis.c_str())
                                                         : -1;
                    done = set(axes, at, static_cast<Sint16>(way == '+' ? 32767 : -32768));
                }
                else if (c.rfind("button", 0) == 0)
                    done = set(buttons, std::atoi(c.c_str() + 6), true);
                else if (c.rfind("hat", 0) == 0 && c.size() > 4)
                {
                    const std::string dir = c.substr(4);
                    const uint8_t bit = dir == "up" ? SDL_HAT_UP : dir == "down" ? SDL_HAT_DOWN
                                      : dir == "left" ? SDL_HAT_LEFT : dir == "right" ? SDL_HAT_RIGHT : 0;
                    done = bit && set(hats, c[3] - '0', bit);
                }
                else if (v.gamepad)
                    done = set(buttons, SDL_GetGamepadButtonFromString(c.c_str()), true);
                if (!done)
                    std::cerr << "[input] " << v.name << ": no control " << c << std::endl;
            }
            for (size_t i = 0; i < buttons.size(); ++i)
                SDL_SetJoystickVirtualButton(joystick, static_cast<int>(i), buttons[i]);
            for (size_t i = 0; i < axes.size(); ++i)
                SDL_SetJoystickVirtualAxis(joystick, static_cast<int>(i), axes[i]);
            for (size_t i = 0; i < hats.size(); ++i)
                SDL_SetJoystickVirtualHat(joystick, static_cast<int>(i), hats[i]);
        }
    }

    bool syncVirtualControllers()
    {
        std::vector<Virtual> wanted;
        {
            std::lock_guard<std::mutex> lock(s_mutex);
            wanted = s_virtuals;
        }
        bool changed = false;
        for (auto at = s_attached.begin(); at != s_attached.end();)
        {
            if (std::any_of(wanted.begin(), wanted.end(), [&](const Virtual &v) { return v.name == at->first; }))
            {
                ++at;
                continue;
            }
            SDL_CloseJoystick(at->second.joystick);
            SDL_DetachVirtualJoystick(at->second.id);
            at = s_attached.erase(at);
            changed = true;
        }
        for (const Virtual &v : wanted)
        {
            auto at = s_attached.find(v.name);
            if (at == s_attached.end())
            {
                // A hidden run opens no input, so SDL's is started here,
                // its window never focused.
                if (!SDL_WasInit(SDL_INIT_GAMEPAD))
                {
                    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
                    if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD))
                    {
                        std::cerr << "[input] init: " << SDL_GetError() << std::endl;
                        return changed;
                    }
                }
                at = s_attached.emplace(v.name, Attached{v.name}).first;
                SDL_VirtualJoystickDesc desc;
                SDL_INIT_INTERFACE(&desc);
                desc.type = static_cast<Uint16>(v.type);
                desc.vendor_id = v.vendor;
                desc.product_id = v.product;
                desc.name = at->second.name.c_str();
                if (v.gamepad)
                {
                    desc.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
                    desc.naxes = SDL_GAMEPAD_AXIS_COUNT;
                    desc.button_mask = (1u << SDL_GAMEPAD_BUTTON_COUNT) - 1u;
                    desc.axis_mask = (1u << SDL_GAMEPAD_AXIS_COUNT) - 1u;
                }
                else
                {
                    desc.nbuttons = 16;
                    desc.naxes = 6;
                    desc.nhats = 1;
                }
                at->second.id = SDL_AttachVirtualJoystick(&desc);
                at->second.joystick = at->second.id ? SDL_OpenJoystick(at->second.id) : nullptr;
                if (!at->second.joystick)
                {
                    std::cerr << "[input] " << v.name << ": " << SDL_GetError() << std::endl;
                    if (at->second.id)
                        SDL_DetachVirtualJoystick(at->second.id);
                    s_attached.erase(at);
                    continue;
                }
                changed = true;
            }
            hold(at->second.joystick, v);
        }
        if (!s_attached.empty())
            SDL_UpdateJoysticks();
        return changed;
    }

    // A change to a device's layout stays in this run.
    void syncScriptedDevices()
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        std::erase_if(devices(), [&](const Device &d) {
            return d.id.backend == DeviceId::kScripted &&
                   std::none_of(s_scripted.begin(), s_scripted.end(),
                                [&](const Scripted &s) { return s.name == d.id.model; });
        });
        for (const Scripted &s : s_scripted)
        {
            const DeviceId id{DeviceId::kScripted, s.name, ""};
            Device *d = deviceById(id);
            if (!d)
            {
                Device made;
                made.id = id;
                made.name = s.name;
                // _ in its name is a space (Steam_Controller).
                made.label = s.name;
                std::replace(made.label.begin(), made.label.end(), '_', ' ');
                made.detected = s.kind;
                made.sectionName = "script " + s.name;
                made.kind = kindOf(made.sectionName, "", made.detected);
                made.keepInMemory = true;
                made.builtin = scriptedLayout();
                made.profile = overlay(made.builtin, sectionOf(made.sectionName, ""));
                made.rest = scriptedRest();
                made.openedMs = SDL_GetTicks();
                devices().push_back(made);
                d = &devices().back();
            }
            d->now = s.raw;
        }
    }

    bool scriptedKeys(std::array<bool, SDL_SCANCODE_COUNT> &keys)
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        keys = s_keys;
        return s_keysHeld;
    }
}

namespace gh2
{
    using namespace input;

    void addScriptedDevice(const std::string &name)
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        for (const Scripted &d : s_scripted)
            if (d.name == name)
                return;
        s_scripted.push_back(
            {name, name.find("pad") != std::string::npos ? DeviceKind::kGamepad : DeviceKind::kGuitar, scriptedRest()});
    }

    void removeScriptedDevice(const std::string &name)
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        std::erase_if(s_scripted, [&](const Scripted &d) { return d.name == name; });
    }

    void setScriptedDevice(const std::string &name, uint16_t pressed, const std::vector<int> &buttons)
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        for (Scripted &d : s_scripted)
            if (d.name == name)
            {
                d.raw = scriptedRest();
                for (int a = 0; a < kActionCount; ++a)
                    d.raw.buttons[static_cast<size_t>(a)] = (pressed & kActions[a].button) != 0u;
                for (int b : buttons)
                    if (b >= 0 && static_cast<size_t>(b) < kScriptedButtons)
                        d.raw.buttons[static_cast<size_t>(b)] = true;
            }
    }

    void addVirtualController(const std::string &name, uint16_t vendor, uint16_t product, int type)
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        if (std::none_of(s_virtuals.begin(), s_virtuals.end(), [&](const Virtual &v) { return v.name == name; }))
            s_virtuals.push_back({name, vendor, product, type, type == SDL_JOYSTICK_TYPE_GAMEPAD, {}});
    }

    void removeVirtualController(const std::string &name)
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        std::erase_if(s_virtuals, [&](const Virtual &v) { return v.name == name; });
    }

    void setVirtualController(const std::string &name, const std::vector<std::string> &controls)
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        for (Virtual &v : s_virtuals)
            if (v.name == name)
                v.controls = controls;
    }

    bool hasVirtualController(const std::string &name)
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        return std::any_of(s_virtuals.begin(), s_virtuals.end(), [&](const Virtual &v) { return v.name == name; });
    }

    void setScriptedKey(int scancode, bool down)
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        if (scancode <= SDL_SCANCODE_UNKNOWN || scancode >= SDL_SCANCODE_COUNT)
            return;
        s_keys[static_cast<size_t>(scancode)] = down;
        s_keysHeld = true;
    }
}
