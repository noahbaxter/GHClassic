// The keyboard and every connected controller, read through SDL, each into
// a seat's PS2 guitar.
//
// Each poll reads every device into its actions (sdl_devices.cpp, the
// keyboard through its layout in layouts.cpp, a scenario's as scripted),
// moves the seats by them (seats.cpp), then gives each seat's port what its
// device holds. A seat reads one device and nothing else, so a controller
// nobody holds (a resting whammy, a drifting stick) never plays.
//
// The actions land on the RedOctane guitar's buttons (see pad.cpp for why the
// pad is one): frets R2 Circle Triangle Cross Square, the guitar slots in
// config/beatmatch_controller.dta; strum on the D-pad; star power on Select
// (force_mercury); tilt on L2, the mercury_switch GuitarController defaults
// to when the guitar config names none (kPad_L2 is 0, 0x2364d8); whammy on
// the left stick's Y pushed up, the half GetWhammyBar reads (0x236718).

#include "input.h"

#include "dev/timing.h"
#include "host/bindings.h"
#include "host/device.h"
#include "host/layouts.h"
#include "host/owner.h"
#include "host/sdl_devices.h"
#include "host/seats.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <iostream>
#include <mutex>
#include <string>
#include <vector>

namespace gh2
{
    namespace
    {
        using namespace input;

        bool s_open = false;
        Seats s_seats; // the host thread's own
        std::atomic<bool> s_locked{false};
        // The seats as the last poll left them, for any thread.
        std::atomic<bool> s_away[kSeats] = {};   // a seated device not plugged in
        std::atomic<bool> s_filled[kSeats] = {}; // a device in it, for seatPlugged

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

        // A scenario's devices into the list, each as it holds its controls
        // now.
        void syncScripted(const std::vector<Scripted> &scripted)
        {
            std::erase_if(devices(), [&](const Device &d) {
                return d.id.backend == DeviceId::kScripted &&
                       std::none_of(scripted.begin(), scripted.end(),
                                    [&](const Scripted &s) { return s.name == d.id.model; });
            });
            for (const Scripted &s : scripted)
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
                    made.kind = s.kind;
                    made.sectionName = "script " + s.name;
                    made.builtin = scriptedLayout();
                    made.profile = overlay(made.builtin, sectionOf(made.sectionName, ""));
                    made.openedMs = SDL_GetTicks();
                    devices().push_back(made);
                    d = &devices().back();
                }
                d->now = s.raw;
            }
        }

        std::mutex s_mutex;
        HostPad s_pads[kSeats];
        std::string s_seatNames[kSeats]; // for seatHas
        std::vector<Scripted> s_scriptedDevices;
        bool s_flying = false;
        HostFly s_fly, s_scriptedFly;
        uint64_t s_pressEventNs = 0; // the host thread's own
        bool s_lockedWas = false;    // the host thread's

        HostPad padOf(const std::array<float, kActionCount> &held)
        {
            HostPad pad;
            for (int a = 0; a < kActionCount; ++a)
                if (held[a] > kHeld)
                    pad.pressed |= kActions[a].button;
            pad.ly = static_cast<uint8_t>(std::lround(128.0f * (1.0f - held[kWhammy])));
            return pad;
        }

        Read keyboardRead(const bool *keys)
        {
            Read r{DeviceId::keyboard(), "keyboard", DeviceKind::kKeyboard, "keyboard"};
            play(keyboardLayout(), Snapshot{}, keys, r.held);
            return r;
        }
    }

    void openInput()
    {
        // The controller keeps playing while the window is behind the
        // terminal that launched it.
        SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
        if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD))
        {
            std::cerr << "[input] init: " << SDL_GetError() << std::endl;
            return;
        }
        loadLayouts();
        s_seats.restore(layoutFile().seats, SDL_GetTicks() + 3000u);
        s_open = true;
        openDevices();
        claimControllers();
    }

    void pollInput(bool devicesChanged)
    {
        // A hidden run has no input open, but its scenario's devices play.
        const bool *keys = nullptr;
        std::vector<Read> reads;
        bool owner = false;
        if (s_open)
        {
            if (devicesChanged)
                openDevices();
            owner = ownsControllers();
            keys = SDL_GetKeyboardState(nullptr);
            if (owner)
            {
                reads.push_back(keyboardRead(keys));
                readSdlDevices();
            }
        }
        {
            std::lock_guard<std::mutex> lock(s_mutex);
            syncScripted(s_scriptedDevices);
        }
        // A window without the controllers reads none of SDL's.
        for (Device &d : devices())
            if (d.id.backend != DeviceId::kSdl || owner)
            {
                nameControls(d);
                Read r{d.id, d.name, d.kind, d.label};
                play(d.profile, d.now, keys, r.held);
                reads.push_back(r);
            }
        for (Read &r : reads)
            r.buttons = padOf(r.held).pressed;

        const bool locked = s_locked.load(std::memory_order_relaxed);
        s_seats.seat(reads, locked ? Mode::kSong : Mode::kMenu, SDL_GetTicks());

        // A window without the controllers reads none, and pauses for none.
        s_seats.follow(reads, s_open && !owner);
        // Each seat's device this poll, none while it is away.
        const Read *seated[kSeats] = {};
        for (const Read &r : reads)
            if (const int seat = s_seats.seatOf(r.id); seat >= 0)
                seated[seat] = &r;

        // What each player plays a song on, for scores filed by it later.
        if (locked && !s_lockedWas)
            for (int seat = 0; seat < kSeats; ++seat)
                if (seated[seat])
                    std::cerr << "[input] song: P" << seat + 1 << " on " << kindName(seated[seat]->kind) << std::endl;
        s_lockedWas = locked;

        // A device just seated plays from when it lets go of what it held.
        HostPad pads[kSeats];
        for (int seat = 0; seat < kSeats; ++seat)
            if (seated[seat] && !s_seats.holding(*seated[seat]))
                pads[seat] = padOf(seated[seat]->held);

        if (s_open)
            if (const auto saved = s_seats.takeSaved(); saved && !writeSeats(bindingFilePath(), *saved))
                std::cerr << "[input] could not write " << bindingFilePath() << std::endl;
        for (int seat = 0; seat < kSeats; ++seat)
        {
            s_away[seat].store(s_seats.away(seat), std::memory_order_relaxed);
            s_filled[seat].store(!s_seats[seat].none(), std::memory_order_relaxed);
        }

        // Read every poll, so the travel from before a flight is not its
        // first step.
        float mouseX = 0.0f, mouseY = 0.0f;
        if (s_open)
            SDL_GetRelativeMouseState(&mouseX, &mouseY);
        std::lock_guard<std::mutex> lock(s_mutex);
        for (int seat = 0; seat < kSeats; ++seat)
            s_seatNames[seat] = seated[seat] ? seated[seat]->name : "";
        HostPad &pad = pads[0];
        pad.pressCount = s_pads[0].pressCount;
        pad.pressEventNs = s_pads[0].pressEventNs;
        pad.pressPollNs = s_pads[0].pressPollNs;
        if (timing::on() && (pad.pressed & ~s_pads[0].pressed))
        {
            ++pad.pressCount;
            pad.pressPollNs = timing::now();
            // An event from before the last poll is another press's.
            pad.pressEventNs = s_pressEventNs > s_pads[0].pressPollNs ? s_pressEventNs : 0u;
            if (pad.pressEventNs != 0u)
                timing::span(timing::kEventToPoll, pad.pressEventNs, pad.pressPollNs);
        }
        std::copy(std::begin(pads), std::end(pads), std::begin(s_pads));
        if (s_flying && keys)
        {
            s_fly.right = (keys[SDL_SCANCODE_D] ? 1.0f : 0.0f) - (keys[SDL_SCANCODE_A] ? 1.0f : 0.0f);
            s_fly.forward = (keys[SDL_SCANCODE_W] ? 1.0f : 0.0f) - (keys[SDL_SCANCODE_S] ? 1.0f : 0.0f);
            s_fly.fast = keys[SDL_SCANCODE_LSHIFT];
            s_fly.rollLeft = keys[SDL_SCANCODE_Q];
            s_fly.rollRight = keys[SDL_SCANCODE_E];
            s_fly.mouseX += mouseX;
            s_fly.mouseY += mouseY;
        }
    }

    void notePressEvent(uint64_t ns)
    {
        s_pressEventNs = ns;
    }

    void closeInput()
    {
        if (!s_open)
            return;
        closeDevices();
        SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
        s_open = false;
        std::lock_guard<std::mutex> lock(s_mutex);
        std::fill(std::begin(s_pads), std::end(s_pads), HostPad{});
    }

    HostPad hostPad(int seat)
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        return seat >= 0 && seat < kSeats ? s_pads[seat] : HostPad{};
    }

    bool seatPlugged(int seat)
    {
        if (seat < 0 || seat >= kSeats)
            return false;
        // A seated device gone mid-song: GH2 pauses for it until it is back.
        // Outside one the next press takes the seat instead.
        if (s_locked.load(std::memory_order_relaxed) && s_away[seat].load(std::memory_order_relaxed))
            return false;
        // P1's port always reads a guitar, so a menu answers before anyone
        // has pressed; the rest read one once a player sits there.
        return seat == 0 || s_filled[seat].load(std::memory_order_relaxed);
    }

    void lockSeats(bool locked)
    {
        s_locked.store(locked, std::memory_order_relaxed);
    }

    void addScriptedDevice(const std::string &name)
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        for (const Scripted &d : s_scriptedDevices)
            if (d.name == name)
                return;
        s_scriptedDevices.push_back(
            {name, name.find("pad") != std::string::npos ? DeviceKind::kGamepad : DeviceKind::kGuitar, scriptedRest()});
    }

    void removeScriptedDevice(const std::string &name)
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        std::erase_if(s_scriptedDevices, [&](const Scripted &d) { return d.name == name; });
    }

    void setScriptedDevice(const std::string &name, uint16_t pressed)
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        for (Scripted &d : s_scriptedDevices)
            if (d.name == name)
            {
                d.raw = scriptedRest();
                for (int a = 0; a < kActionCount; ++a)
                    d.raw.buttons[static_cast<size_t>(a)] = (pressed & kActions[a].button) != 0u;
            }
    }

    bool seatHas(int seat, const std::string &name)
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        return seat >= 0 && seat < kSeats && !name.empty() && s_seatNames[seat] == name;
    }

    void setFlying(bool flying)
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        s_flying = flying;
        s_fly = HostFly{};
    }

    bool flying()
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        return s_flying;
    }

    HostFly takeFly()
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        HostFly fly = s_fly;
        fly.right = std::clamp(fly.right + s_scriptedFly.right, -1.0f, 1.0f);
        fly.forward = std::clamp(fly.forward + s_scriptedFly.forward, -1.0f, 1.0f);
        fly.mouseX += s_scriptedFly.mouseX;
        fly.mouseY += s_scriptedFly.mouseY;
        fly.fast |= s_scriptedFly.fast;
        fly.rollLeft |= s_scriptedFly.rollLeft;
        fly.rollRight |= s_scriptedFly.rollRight;
        s_fly.mouseX = s_fly.mouseY = 0.0f;
        s_scriptedFly.mouseX = s_scriptedFly.mouseY = 0.0f;
        return fly;
    }

    void setScriptedFly(const HostFly &fly)
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        s_scriptedFly = fly;
    }
}
