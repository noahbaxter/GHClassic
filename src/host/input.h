#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace gh2
{
    // The controller the player holds, as libpad reports a pad: pressed
    // buttons active high in libpad's bit order, sticks centred on 0x80.
    struct HostPad
    {
        uint16_t pressed = 0;
        uint8_t lx = 0x80, ly = 0x80, rx = 0x80, ry = 0x80;
        // --timing: the last poll that saw a button go down, counted, with
        // the OS's time for the press (0 if it gave none) and the poll's.
        uint32_t pressCount = 0;
        uint64_t pressEventNs = 0, pressPollNs = 0;
    };

    // --timing: the OS's time on a button, key, hat or axis event, for the
    // next poll that sees a press.
    void notePressEvent(uint64_t ns);

    // libpad's button bits.
    namespace pad
    {
        constexpr uint16_t kSelect = 1u << 0, kL3 = 1u << 1, kR3 = 1u << 2, kStart = 1u << 3;
        constexpr uint16_t kUp = 1u << 4, kRight = 1u << 5, kDown = 1u << 6, kLeft = 1u << 7;
        constexpr uint16_t kL2 = 1u << 8, kR2 = 1u << 9, kL1 = 1u << 10, kR1 = 1u << 11;
        constexpr uint16_t kTriangle = 1u << 12, kCircle = 1u << 13, kCross = 1u << 14, kSquare = 1u << 15;
    }

    // The frontend's side, on the thread that pumps SDL events. Poll after
    // each pump; `devicesChanged` when a joystick came or went.
    void openInput();
    void pollInput(bool devicesChanged);
    void closeInput();

    // Of the GHClassic windows open, the first up, or else the last focused,
    // reads the controllers, focused or not; the others read nothing
    // pressed. A hidden run opens no input at all. Call when this window
    // gains focus.
    void takeControllers();
    // A scripted run never takes them, at launch or on focus: a window that
    // opens over the player's (as a new one does on KDE) leaves their
    // controllers alone. Before openInput.
    void leaveControllers();

    // Up to four players; GH2 has two ports' worth of screens, so two join.
    constexpr int kSeats = 4;
    constexpr int kPlayers = 2;

    // A seat's controller as the last poll saw it, idle when no device is
    // in it. Seat N is libpad's port N. Callable from any thread.
    HostPad hostPad(int seat = 0);
    // Whether the game sees a controller in that seat's port.
    bool seatPlugged(int seat);
    // While a song plays, a press on another device takes no seat.
    // Callable from any thread.
    void lockSeats(bool locked);
    // Which screen of a song is up: in one playing, the keyboard's menu
    // keys would be a guitar's frets and strums, so only its Esc works,
    // as Start; in its pause menu Esc is Start too, to resume. Callable
    // from any thread.
    enum class SongScreen
    {
        kOther,
        kPlaying,
        kPaused,
    };
    void songScreen(SongScreen screen);

    // A scenario's controllers that are not plugged in, by name. Each plays
    // as a real one does, through its layout, the seat rules and the
    // Controllers screen, in a hidden run too. Held until the next set, none
    // to let go: the controls its layout gives the actions of those libpad
    // bits, and raw buttons by number. Callable from any thread.
    void addScriptedDevice(const std::string &name);
    void removeScriptedDevice(const std::string &name); // unplugged
    void setScriptedDevice(const std::string &name, uint16_t pressed, const std::vector<int> &buttons = {});
    // A scenario's key, SDL's scancode, held on the keyboard until let go;
    // in a hidden run, from the first, the keyboard is there. Callable from
    // any thread.
    void setScriptedKey(int scancode, bool down);
    // Whether that seat reads the device of that name: a scripted one's, or
    // a real one's as the log names it. Callable from any thread.
    bool seatHas(int seat, const std::string &name);
    // What a player is shown for it: its guitar's layout ("Wii guitar
    // (raphnet)"), else its name; "" for none. Callable from any thread.
    std::string seatLabel(int seat);

    // Whether Multiplayer's screen is up, where a controller joins with
    // Start and yellow moves a player up a seat. Callable from any thread.
    void setPlayersStep(bool on);
    // How many times P1 and P2 have traded places, by yellow there or by
    // P2 moving up when P1's controller is let go. Callable from any thread.
    uint32_t seatTrades();

    // The free camera's controls (ui/photo_mode.h): W A S D as how far it
    // is pushed right and forward, -1 to 1, the mouse's travel in pixels,
    // right and down, left shift for speed, and Q and E to roll it left
    // and right.
    struct HostFly
    {
        float right = 0.0f, forward = 0.0f;
        float mouseX = 0.0f, mouseY = 0.0f;
        bool fast = false, rollLeft = false, rollRight = false;
    };

    // While flying the frontend keeps the mouse to its window, and each poll
    // reads the keys and adds the mouse's travel. Callable from any thread.
    void setFlying(bool flying);
    bool flying();
    // The keys as the last poll saw them, and the travel since the last take.
    HostFly takeFly();
    // A scripted run's: the keys held on top of the player's until the
    // next, and travel added once.
    void setScriptedFly(const HostFly &fly);
}
