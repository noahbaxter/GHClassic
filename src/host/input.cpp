// The keyboard and every connected controller, read through SDL, each into
// a seat's PS2 guitar.
//
// Each poll reads every device into its actions (sdl_devices.cpp, the
// keyboard through its layout in layouts.cpp, a scenario's in
// scripted_devices.cpp), moves the seats by them (seats.cpp), or on the
// Controllers screen shows them there instead (device_screen.cpp), then
// gives each seat's port what its device holds. A seat reads one device and nothing else, so a
// controller nobody holds (a resting whammy, a drifting stick) never plays.
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
#include "host/device_screen.h"
#include "host/layouts.h"
#include "host/owner.h"
#include "host/scripted_devices.h"
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
        std::atomic<bool> s_playersStep{false};
        std::atomic<SongScreen> s_songScreen{SongScreen::kOther};
        // The seats as the last poll left them, for any thread.
        std::atomic<bool> s_away[kSeats] = {};   // a seated device not plugged in
        std::atomic<bool> s_filled[kSeats] = {}; // a device in it, for seatPlugged
        std::atomic<uint32_t> s_trades{0};

        std::mutex s_mutex;
        HostPad s_pads[kSeats];
        std::string s_seatNames[kSeats]; // for seatHas
        std::string s_seatLabels[kSeats];
        bool s_flying = false;
        HostFly s_fly, s_scriptedFly;
        uint64_t s_pressEventNs = 0; // the host thread's own

        // The keyboard in a menu, whoever holds it: the arrows move, Enter
        // picks and Esc backs out, as green and red. Unseated, these drive
        // P1's port, so a guitar player always has the keyboard too.
        struct MenuKey
        {
            SDL_Scancode key;
            uint16_t bit;
        };
        const MenuKey kMenuKeys[] = {
            {SDL_SCANCODE_UP, pad::kUp},       {SDL_SCANCODE_DOWN, pad::kDown},
            {SDL_SCANCODE_LEFT, pad::kLeft},   {SDL_SCANCODE_RIGHT, pad::kRight},
            {SDL_SCANCODE_RETURN, pad::kR2},   {SDL_SCANCODE_ESCAPE, pad::kCircle},
        };
        // The host thread's: the mode last poll, and the menu keys held as
        // the mode changed, which do nothing until let go: the Enter that
        // paused would pick Resume, the Escape that left the Controllers
        // screen would back out of the menu under it.
        Mode s_modeWas = Mode::kMenu;
        bool s_joypad[kSeats] = {}; // each seat playing this song as a DualShock
        std::array<bool, SDL_SCANCODE_COUNT> s_keyWait{};

        // What is up, from what each part of the game says of it: the
        // Controllers screen over a song, a song over Multiplayer's screen.
        Mode currentMode()
        {
            if (controllersScreen())
                return Mode::kControllers;
            if (s_locked.load(std::memory_order_relaxed))
                switch (s_songScreen.load(std::memory_order_relaxed))
                {
                case SongScreen::kPlaying:
                    return Mode::kPlaying;
                case SongScreen::kPaused:
                    return Mode::kPaused;
                default:
                    return Mode::kSong;
                }
            return s_playersStep.load(std::memory_order_relaxed) ? Mode::kPlayers : Mode::kMenu;
        }

        // Each action on a DualShock, as GH2's joypad layout takes it
        // (config/beatmatch_controller.dta, joypad): frets L2 L1 R1 R2 X,
        // star power on Select; tilt is a guitar's alone.
        constexpr uint16_t kJoypadButtons[kActionCount] = {
            pad::kL2, pad::kL1, pad::kR1, pad::kR2, pad::kCross, pad::kUp, pad::kDown, pad::kStart, pad::kSelect, 0, 0,
        };

        // The guitar's buttons, or with `joypad` a DualShock's.
        HostPad padOf(const std::array<float, kActionCount> &held, bool joypad = false)
        {
            HostPad pad;
            pad.joypad = joypad;
            for (int a = 0; a < kActionCount; ++a)
                if (held[a] > kHeld)
                    pad.pressed |= joypad ? kJoypadButtons[a] : kActions[a].button;
            pad.ly = static_cast<uint8_t>(std::lround(128.0f * (1.0f - held[kWhammy])));
            return pad;
        }

        Read keyboardRead(const bool *keys)
        {
            Read r{DeviceId::keyboard(), "keyboard", DeviceKind::kKeyboard, "keyboard"};
            play(keyboardLayout(), Snapshot{}, keys, r.held);
            r.tab = keys[SDL_SCANCODE_TAB];
            r.escape = keys[SDL_SCANCODE_ESCAPE];
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
        // A hidden run has no input open, but its scenario's devices play,
        // and its keys, as the keyboard.
        const bool *keys = nullptr;
        std::array<bool, SDL_SCANCODE_COUNT> scriptedKeys{};
        const bool keysScripted = input::scriptedKeys(scriptedKeys);
        // A scenario's SDL controllers play in any run, the player's only
        // in one with input open.
        const bool virtualChanged = syncVirtualControllers();
        if (!s_open && virtualChanged)
            openDevices(true);
        std::vector<Read> reads;
        const bool owner = s_open && ownsControllers();
        if (!s_open && keysScripted)
        {
            keys = scriptedKeys.data();
            reads.push_back(keyboardRead(keys));
        }
        if (s_open && (devicesChanged || virtualChanged))
            openDevices();
        if (!owner)
            readSdlDevices(true);
        if (s_open)
        {
            keys = SDL_GetKeyboardState(nullptr);
            if (keysScripted)
            {
                for (int k = 0; k < SDL_SCANCODE_COUNT; ++k)
                    scriptedKeys[static_cast<size_t>(k)] = scriptedKeys[static_cast<size_t>(k)] || keys[k];
                keys = scriptedKeys.data();
            }
            if (owner)
            {
                reads.push_back(keyboardRead(keys));
                readSdlDevices();
            }
        }
        syncScriptedDevices();
        // A window without the controllers reads none of SDL's, but a
        // scenario's.
        for (Device &d : devices())
            if (d.id.backend != DeviceId::kSdl || owner || d.isVirtual)
            {
                nameControls(d);
                Read r{d.id, d.name, d.kind, d.label};
                play(d.profile, d.now, keys, r.held);
                reads.push_back(r);
            }
        for (Read &r : reads)
            r.buttons = padOf(r.held).pressed;

        // On the Controllers screen devices only show there. Leaving it,
        // nothing held there plays until let go: red held to leave would
        // back out of the menu under it.
        const Mode mode = currentMode();
        if (mode == Mode::kControllers)
            screenPoll(reads, keys, s_modeWas != Mode::kControllers, s_seats);
        s_seats.seat(reads, mode, SDL_GetTicks());
        if (s_modeWas == Mode::kControllers && mode != Mode::kControllers)
            s_seats.letGoAll();

        // A window without the controllers reads none, and pauses for none.
        s_seats.follow(reads, s_open && !owner);
        // Each seat's device this poll, none while it is away.
        const Read *seated[kSeats] = {};
        for (const Read &r : reads)
            if (const int seat = s_seats.seatOf(r.id); seat >= 0)
                seated[seat] = &r;

        // What each player plays a song on, for scores filed by it later.
        if (inSong(mode) && !inSong(s_modeWas))
            for (int seat = 0; seat < kSeats; ++seat)
                if (seated[seat])
                    std::cerr << "[input] song: P" << seat + 1 << " on " << kindName(seated[seat]->kind) << std::endl;

        // A device just seated, or just off the Controllers screen, plays
        // from when it lets go of what it held.
        // A gamepad's seat plays the song as a DualShock, decided as it
        // starts and kept to its end, its controller away or not.
        if (!inSong(mode))
            std::fill(std::begin(s_joypad), std::end(s_joypad), false);
        else if (!inSong(s_modeWas))
            for (int seat = 0; seat < kSeats; ++seat)
                s_joypad[seat] = seated[seat] && seated[seat]->kind == DeviceKind::kGamepad;
        HostPad pads[kSeats];
        for (int seat = 0; seat < kSeats; ++seat)
        {
            if (seated[seat] && !s_seats.holding(*seated[seat]))
                pads[seat] = padOf(seated[seat]->held, s_joypad[seat]);
            pads[seat].joypad = s_joypad[seat];
        }

        // The keyboard's menu keys, on its own seat's port or else P1's, its
        // Enter there no Start. In a song playing only its Esc, as Start,
        // and in the song's pause menu Esc is Start too.
        for (const Read &r : reads)
            if (r.keyboard() && !s_seats.waiting(r.id))
            {
                const int seat = s_seats.seatOf(r.id);
                HostPad &to = pads[seat < 0 ? 0 : seat];
                if (seat >= 0 && mode != Mode::kPlaying)
                    to.pressed &= static_cast<uint16_t>(~pad::kStart);
                for (const MenuKey &k : kMenuKeys)
                {
                    const bool esc = k.key == SDL_SCANCODE_ESCAPE;
                    if (mode != s_modeWas && keys[k.key])
                        s_keyWait[k.key] = true;
                    if (mode == Mode::kPlaying && !esc)
                        continue;
                    if (!keys[k.key])
                        s_keyWait[k.key] = false;
                    else if (!s_keyWait[k.key])
                        to.pressed |= esc && (mode == Mode::kPlaying || mode == Mode::kPaused) ? pad::kStart
                                      // A DualShock picks with X and backs out with Triangle.
                                      : to.joypad && k.bit == pad::kR2      ? pad::kCross
                                      : to.joypad && k.bit == pad::kCircle  ? pad::kTriangle
                                                                            : k.bit;
                }
            }
        s_modeWas = mode;
        if (mode == Mode::kControllers)
            std::fill(std::begin(pads), std::end(pads), HostPad{});

        // A window without the controllers, a scripted one's among them,
        // leaves last run's seats alone.
        if (owner)
            if (const auto saved = s_seats.takeSaved(); saved && !writeSeats(bindingFilePath(), *saved))
                std::cerr << "[input] could not write " << bindingFilePath() << std::endl;
        for (int seat = 0; seat < kSeats; ++seat)
        {
            s_away[seat].store(s_seats.away(seat), std::memory_order_relaxed);
            s_filled[seat].store(!s_seats[seat].none(), std::memory_order_relaxed);
        }
        s_trades.store(s_seats.trades(), std::memory_order_relaxed);

        // Read every poll, so the travel from before a flight is not its
        // first step.
        float mouseX = 0.0f, mouseY = 0.0f;
        if (s_open)
            SDL_GetRelativeMouseState(&mouseX, &mouseY);
        std::lock_guard<std::mutex> lock(s_mutex);
        for (int seat = 0; seat < kSeats; ++seat)
        {
            s_seatNames[seat] = seated[seat] ? seated[seat]->name : "";
            s_seatLabels[seat] = seated[seat] ? seated[seat]->label : "";
        }
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
        // has pressed; the rest read one once a player sits there, which is
        // when GH2 offers Multiplayer.
        return seat == 0 || s_filled[seat].load(std::memory_order_relaxed);
    }

    void lockSeats(bool locked)
    {
        s_locked.store(locked, std::memory_order_relaxed);
    }

    void songScreen(SongScreen screen)
    {
        s_songScreen.store(screen, std::memory_order_relaxed);
    }

    bool seatHas(int seat, const std::string &name)
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        return seat >= 0 && seat < kSeats && !name.empty() && s_seatNames[seat] == name;
    }

    std::string seatLabel(int seat)
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        return seat >= 0 && seat < kSeats ? s_seatLabels[seat] : std::string();
    }

    void setPlayersStep(bool on)
    {
        s_playersStep.store(on, std::memory_order_relaxed);
    }

    uint32_t seatTrades()
    {
        return s_trades.load(std::memory_order_relaxed);
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
