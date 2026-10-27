#include "host/device_screen.h"

#include "host/device.h"
#include "host/devices.h"
#include "host/layouts.h"
#include "host/profiles.h"
#include "host/sdl_devices.h"

#include <algorithm>
#include <atomic>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>

namespace gh2::input
{
    namespace
    {
        // What the screen reads and asks for, any thread's under s_mutex;
        // the rest the host thread's own.
        std::atomic<bool> s_screen{false};
        std::mutex s_mutex;
        std::vector<DeviceView> s_views;
        DeviceId s_shown;
        std::vector<int> s_keys;
        float s_redHeld = 0.0f;
        struct RebindAsk
        {
            DeviceId id;
            Action action = kGreen;
            bool asked = false;
        };
        RebindAsk s_rebindAsk;
        bool s_rebinding = false; // asked and not yet bound or let go
        struct BindingEdit
        {
            DeviceId id;
            Action action = kGreen;
            bool reset = false;
        };
        std::vector<BindingEdit> s_edits;
        // The host thread's: keys and devices down last poll, when red went
        // down on the device shown, and the capture under way.
        std::array<bool, SDL_SCANCODE_COUNT> s_keyWas{};
        std::map<DeviceId, bool> s_touchWas;
        uint64_t s_redSinceMs = 0;
        std::optional<Capture> s_capture;
        bool s_caughtNamed = false; // the log has said what it caught

        // A device's section written with `change` made to it, and every
        // device of its model, which share it, playing it from then.
        void rewrite(const DeviceId &id, const std::function<void(Section &)> &change)
        {
            if (id.backend == DeviceId::kKeyboard)
            {
                editSection("", "", keyboardProfile(), false, change);
                return;
            }
            const Device *d = deviceById(id);
            if (!d || !editSection(d->sectionName, d->sectionGuid, d->builtin, d->keepInMemory, change))
                return;
            const std::string name = d->sectionName, guid = d->sectionGuid;
            for (Device &same : devices())
                if (same.sectionName == name && same.sectionGuid == guid)
                    same.profile = overlay(same.builtin, sectionOf(name, guid));
        }

        // A device that went from nothing pressed to something: a bound
        // action, or any control off its rest, so one with nothing bound
        // still shows.
        bool touching(const Read &r)
        {
            for (float held : r.held)
                if (held > kHeld)
                    return true;
            const Device *d = deviceById(r.id);
            return d && moved(d->rest, d->now);
        }
    }

    bool controllersScreen()
    {
        return s_screen.load(std::memory_order_relaxed);
    }

    void screenPoll(const std::vector<Read> &reads, const bool *keys, bool opened, const Seats &seats)
    {
        // Each axis's rest taken again: one read before SDL had the device's
        // first report (a trigger at 0, not -1), or a whammy since settled
        // elsewhere, would read as touched for good.
        if (opened)
            for (Device &d : devices())
                d.rest.axes = d.now.axes;
        std::vector<int> down;
        for (int k = 0; keys && k < SDL_SCANCODE_COUNT; ++k)
        {
            // What was held as the screen came up is not pressed on it.
            if (keys[k] && !s_keyWas[static_cast<size_t>(k)] && !opened)
                down.push_back(k);
            s_keyWas[static_cast<size_t>(k)] = keys[k];
        }
        // What the screen asked for since the last poll, taken under the
        // lock; input.ini is written outside it.
        std::vector<BindingEdit> edits;
        RebindAsk ask;
        bool rebinding = false;
        {
            std::lock_guard<std::mutex> lock(s_mutex);
            for (const Read &r : reads)
            {
                if (r.keyboard())
                    continue;
                const bool now = touching(r);
                if (now && !s_touchWas[r.id] && !opened)
                    s_shown = r.id;
                s_touchWas[r.id] = now;
            }
            std::erase_if(s_touchWas, [&](const auto &was) {
                return std::none_of(reads.begin(), reads.end(), [&](const Read &r) { return r.id == was.first; });
            });
            edits.swap(s_edits);
            ask = s_rebindAsk;
            rebinding = s_rebinding;
            s_rebindAsk.asked = false;
        }

        for (const BindingEdit &e : edits)
            rewrite(e.id, [&](Section &lines) {
                if (e.reset)
                    lines.clear();
                else
                    lines[e.action] = {};
            });

        if (!rebinding)
            s_capture.reset();
        else if (ask.asked)
        {
            s_capture.reset();
            if (Device *d = deviceById(ask.id))
            {
                // Tilt may be the accelerometer.
                if (ask.action == kTilt)
                    enableAccel(*d);
                s_capture.emplace(d->now, ask.action == kWhammy ? Capture::kAxisOnly
                                          : ask.action == kTilt ? Capture::kTilt
                                                                : Capture::kAny);
                s_caughtNamed = false;
                std::cerr << "[input] " << d->name << ": waiting for a control for " << kActions[ask.action].name
                          << std::endl;
            }
        }
        bool done = false;
        if (rebinding)
        {
            std::optional<Source> got;
            Device *d = deviceById(ask.id);
            const bool keyboard = ask.id.backend == DeviceId::kKeyboard;
            if (keyboard)
            {
                for (auto k = down.begin(); k != down.end(); ++k)
                    if (*k != SDL_SCANCODE_ESCAPE)
                    {
                        got = Source{Source::kKey, *k};
                        // The key bound does nothing else on the screen.
                        down.erase(k);
                        break;
                    }
            }
            else if (d && s_capture && s_capture->feed(d->now))
                got = s_capture->found();
            // What a player's log needs to say why one never binds.
            else if (d && s_capture && s_capture->found() && !s_caughtNamed)
            {
                s_caughtNamed = true;
                std::cerr << "[input] " << d->name << ": caught " << formatSource(*s_capture->found())
                          << ", waiting for it to be let go" << std::endl;
            }
            if (got)
            {
                std::cerr << "[input] " << (d ? d->name : "keyboard") << ": " << kActions[ask.action].name << " = "
                          << formatSource(*got) << std::endl;
                rewrite(ask.id, [&](Section &lines) { lines[ask.action] = {*got}; });
            }
            // Bound, or its device unplugged.
            done = got || (!d && !keyboard);
            if (done)
                s_capture.reset();
        }

        std::lock_guard<std::mutex> lock(s_mutex);
        // Unless another was asked for meanwhile.
        if (done && !s_rebindAsk.asked && s_rebindAsk.id == ask.id && s_rebindAsk.action == ask.action)
            s_rebinding = false;
        s_keys.insert(s_keys.end(), down.begin(), down.end());

        // Each device as it shows, the keyboard last; the one shown falls to
        // the first controller when it goes.
        const auto model = [](const DeviceId &id) { return DeviceId{id.backend, id.model, ""}; };
        std::map<DeviceId, int> ofModel, nth;
        for (const Device &d : devices())
            ++ofModel[model(d.id)];
        s_views.clear();
        for (const Read &r : reads)
        {
            DeviceView v;
            v.id = r.id;
            v.kind = r.kind;
            v.label = r.keyboard() ? "Keyboard" : r.label;
            const Device *d = deviceById(r.id);
            if (d && ofModel[model(d->id)] > 1)
                v.number = ++nth[model(d->id)];
            const Profile &profile = d ? d->profile : keyboardLayout();
            for (int a = 0; a < kActionCount; ++a)
            {
                v.amount[static_cast<size_t>(a)] = r.held[a];
                v.bound[static_cast<size_t>(a)] = joined(d ? d->gamepad : nullptr, profile[static_cast<size_t>(a)]);
            }
            v.seat = seats.seatOf(v.id);
            s_views.push_back(v);
        }
        std::stable_partition(s_views.begin(), s_views.end(),
                              [](const DeviceView &v) { return v.kind != DeviceKind::kKeyboard; });
        if (std::none_of(s_views.begin(), s_views.end(), [](const DeviceView &v) { return v.id == s_shown; }))
            s_shown = s_views.empty() ? DeviceId{} : s_views.front().id;

        s_redHeld = 0.0f;
        const uint64_t nowMs = SDL_GetTicks();
        // Red alone: held with anything else, it is a chord being tried.
        bool red = false;
        for (const Read &r : reads)
            if (r.id == s_shown && r.held[kRed] > kHeld)
            {
                red = true;
                for (int a = 0; a < kActionCount; ++a)
                    if (a != kRed && r.held[a] > kHeld)
                        red = false;
            }
        if (!red)
            s_redSinceMs = 0;
        else if (s_redSinceMs == 0)
            s_redSinceMs = nowMs;
        else
            s_redHeld = static_cast<float>(nowMs - s_redSinceMs) / 1000.0f;
    }

    void setControllersScreen(bool on)
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        if (on && !s_screen.load(std::memory_order_relaxed))
        {
            s_keys.clear();
            s_rebinding = false;
            s_rebindAsk = {};
        }
        s_screen.store(on, std::memory_order_relaxed);
    }

    std::vector<DeviceView> deviceViews()
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        return s_views;
    }

    DeviceId touchedDevice()
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        return s_shown;
    }

    void showDevice(const DeviceId &id)
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        s_shown = id;
    }

    std::vector<int> takeScreenKeys()
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        std::vector<int> keys;
        keys.swap(s_keys);
        return keys;
    }

    float redHeldSeconds()
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        return s_redHeld;
    }

    void startRebind(const DeviceId &id, Action action)
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        s_rebindAsk = {id, action, true};
        s_rebinding = true;
    }

    void cancelRebind()
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        s_rebindAsk = {};
        s_rebinding = false;
    }

    bool rebinding()
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        return s_rebinding;
    }

    void clearBinding(const DeviceId &id, Action action)
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        s_edits.push_back({id, action, false});
    }

    void resetBindings(const DeviceId &id)
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        s_edits.push_back({id, kGreen, true});
    }
}
