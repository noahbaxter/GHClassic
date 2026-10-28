// Who sits where as devices press: tools/ghc.py ci builds and runs it; it
// exits 1 on any failed check.

#include "host/seats.h"

#include <iostream>
#include <string>
#include <vector>

namespace
{
    using namespace gh2;
    using namespace gh2::input;

    int s_failed = 0;

#define CHECK(condition) \
    do \
    { \
        if (!(condition)) \
        { \
            std::cerr << "failed: " #condition << " (line " << __LINE__ << ")" << std::endl; \
            ++s_failed; \
        } \
    } while (0)

    // A controller as SDL has it, GUID then path.
    DeviceId sdl(const std::string &guid, const std::string &path)
    {
        return {DeviceId::kSdl, guid, path};
    }

    const DeviceId kKeyboard = DeviceId::keyboard();

    // A device holding these libpad buttons.
    Read device(const DeviceId &id, uint16_t buttons = 0)
    {
        Read r{id, id.text(), DeviceKind::kGuitar, id.text()};
        r.buttons = buttons;
        return r;
    }

    Read keyboard(uint16_t buttons = 0)
    {
        Read r{kKeyboard, "keyboard", DeviceKind::kKeyboard, "keyboard"};
        r.buttons = buttons;
        return r;
    }

    // One menu poll, then one with everything let go, so the next press is
    // a new one.
    void press(Seats &seats, std::vector<Read> reads, Mode mode = Mode::kMenu)
    {
        seats.seat(reads, mode, 0);
        for (Read &r : reads)
            r.buttons = 0;
        seats.seat(reads, mode, 0);
    }
}

int main()
{
    const DeviceId a = sdl("guid-a", "/dev/a"), b = sdl("guid-b", "/dev/b"), c = sdl("guid-c", "/dev/c");

    // The first press takes P1; a resting whammy or star power does not.
    {
        Seats seats;
        press(seats, {device(a, pad::kSelect | pad::kL2), device(b)});
        CHECK(seats[0].none());
        press(seats, {device(a), device(b, pad::kCircle)});
        CHECK(seats[0] == b);
    }

    // Alone, a press on another controller takes P1: the one player
    // picking up another; not off a press P1's made in the same poll.
    {
        Seats seats;
        press(seats, {device(a, pad::kR2), device(b)});
        press(seats, {device(a, pad::kR2), device(b, pad::kR2)});
        CHECK(seats[0] == a);
        press(seats, {device(a), device(b, pad::kR2)});
        CHECK(seats[0] == b);
        CHECK(seats[1].none());
    }

    // The keyboard takes no seat, first or after a controller (input.cpp
    // drives P1's port with it).
    {
        Seats seats;
        press(seats, {keyboard(pad::kDown)});
        press(seats, {keyboard(pad::kR2)});
        CHECK(seats[0].none());
        press(seats, {keyboard(), device(a, pad::kR2)});
        CHECK(seats[0] == a);
        press(seats, {keyboard(pad::kCircle), device(a)});
        press(seats, {keyboard(pad::kStart), device(a)});
        CHECK(seats[0] == a && seats.seatOf(kKeyboard) < 0);
    }

    // On Multiplayer's screen Start joins the next free seat, up to
    // kPlayers; a fret there seats nobody.
    {
        Seats seats;
        press(seats, {device(a, pad::kR2), device(b), device(c)}, Mode::kPlayers);
        press(seats, {device(a), device(b, pad::kR2), device(c)}, Mode::kPlayers);
        CHECK(seats[0] == a);
        CHECK(seats[1].none());
        press(seats, {device(a), device(b, pad::kStart), device(c)}, Mode::kPlayers);
        CHECK(seats[1] == b);
        press(seats, {device(a), device(b), device(c, pad::kStart)}, Mode::kPlayers);
        for (int seat = 0; seat < kSeats; ++seat)
            CHECK(seat < kPlayers || seats[seat].none());
        CHECK(kPlayers > 2 || seats.seatOf(c) < 0);
    }

    // Yellow there moves a player up a seat, the first round to the last.
    {
        Seats seats;
        press(seats, {device(a, pad::kR2), device(b)}, Mode::kPlayers);
        press(seats, {device(a), device(b, pad::kStart)}, Mode::kPlayers);
        const uint32_t trades = seats.trades();
        press(seats, {device(a), device(b, pad::kTriangle)}, Mode::kPlayers);
        CHECK(seats[0] == b && seats[1] == a);
        CHECK(seats.trades() == trades + 1);
        press(seats, {device(a), device(b, pad::kTriangle)}, Mode::kPlayers);
        CHECK(seats[0] == a && seats[1] == b);
    }

    // Off that screen Start on another controller takes P1, the one that
    // had it let go.
    {
        Seats seats;
        press(seats, {device(a, pad::kR2), device(b), device(c)}, Mode::kPlayers);
        press(seats, {device(a), device(b, pad::kStart), device(c)}, Mode::kPlayers);
        press(seats, {device(a), device(b), device(c, pad::kStart)});
        CHECK(seats[0] == c && seats[1] == b);
        CHECK(seats.seatOf(a) < 0);
    }

    // Nor does it join on Multiplayer's screen.
    {
        Seats seats;
        press(seats, {device(a, pad::kR2), keyboard()}, Mode::kPlayers);
        press(seats, {device(a), keyboard(pad::kStart)}, Mode::kPlayers);
        CHECK(seats[1].none());
    }

    // Start that seated a device plays nothing until it is let go.
    {
        Seats seats;
        press(seats, {device(a, pad::kR2), device(b)}, Mode::kPlayers);
        seats.seat({device(a), device(b, pad::kStart)}, Mode::kPlayers, 0);
        CHECK(seats.holding(device(b, pad::kStart)));
        CHECK(!seats.holding(device(b)));
        CHECK(!seats.holding(device(b, pad::kStart)));
    }

    // In a song nothing seats. A seat whose controller is gone waits for
    // it: no other controller's fret or Start takes it, nor the keyboard.
    {
        Seats seats;
        press(seats, {device(a, pad::kR2), device(b)});
        press(seats, {device(b, pad::kStart)}, Mode::kPlaying);
        CHECK(seats[0] == a);
        seats.follow({device(b), keyboard()}, false);
        CHECK(seats.away(0));
        press(seats, {device(b, pad::kStart), keyboard(pad::kR2)}, Mode::kPlaying);
        press(seats, {device(b, pad::kR2), keyboard(pad::kStart)}, Mode::kPaused);
        press(seats, {device(b, pad::kR2)}, Mode::kSong);
        CHECK(seats[0] == a && seats.away(0));
    }
    // It comes back as itself on a new path, whatever was pressed while it
    // was gone (a Steam Controller's id changes each time it connects).
    {
        Seats seats;
        const DeviceId again = sdl("guid-a", "/dev/again");
        press(seats, {device(a, pad::kR2), keyboard()});
        seats.follow({keyboard()}, false);
        press(seats, {keyboard(pad::kDown)}, Mode::kPaused);
        CHECK(seats.away(0));
        seats.follow({keyboard(), device(again)}, false);
        CHECK(!seats.away(0) && seats[0] == again);
    }
    // P2's seat waits for P2's own controller as P1's does.
    {
        Seats seats;
        press(seats, {device(a, pad::kR2), device(b)});
        press(seats, {device(a), device(b, pad::kStart)}, Mode::kPlayers);
        CHECK(seats[1] == b);
        seats.follow({device(a), keyboard()}, false);
        CHECK(seats.away(1));
        press(seats, {device(a, pad::kStart), keyboard(pad::kStart)}, Mode::kPaused);
        CHECK(seats[0] == a && seats[1] == b && seats.away(1));
    }

    // A replug back on another path is its seat's again, by GUID.
    {
        Seats seats;
        const DeviceId back = sdl("guid-a", "/dev/new");
        press(seats, {device(a, pad::kR2)});
        seats.follow({}, false);
        CHECK(seats.away(0));
        CHECK(seats[0] == a);
        seats.follow({device(back)}, false);
        CHECK(!seats.away(0));
        CHECK(seats[0] == back);
    }

    // On the Controllers screen nothing moves the seats.
    {
        Seats seats;
        press(seats, {device(a, pad::kR2), device(b)});
        press(seats, {device(a), device(b, pad::kStart)}, Mode::kControllers);
        press(seats, {device(a), device(b, pad::kR2)}, Mode::kControllers);
        CHECK(seats[0] == a && seats[1].none());
    }

    // A window without the controllers pauses for no one.
    {
        Seats seats;
        press(seats, {device(a, pad::kR2)});
        seats.follow({}, true);
        CHECK(!seats.away(0));
    }

    // Last run's seats come back as their devices show up, each its own
    // before one of its model, and are saved again only when they change.
    {
        Seats seats;
        const DeviceId twin = sdl("guid-a", "/dev/twin");
        seats.restore({{"p1", b.text()}, {"p2", a.text()}}, 1000);
        seats.seat({device(twin), device(a), device(b)}, Mode::kMenu, 10);
        CHECK(seats[0] == b && seats[1] == a);
        CHECK(!seats.takeSaved());
        seats.seat({device(a, pad::kTriangle), device(b)}, Mode::kPlayers, 2000);
        const auto saved = seats.takeSaved();
        CHECK(saved && saved->at("p1") == "guid-a /dev/a");
        CHECK(!seats.takeSaved());
    }

    // A saved P1 that shows up after P2 still takes P1, and [seats] is left
    // as it was until they stop being put back.
    {
        Seats seats;
        seats.restore({{"p1", b.text()}, {"p2", a.text()}}, 1000);
        seats.seat({device(a)}, Mode::kMenu, 10);
        CHECK(seats[0].none() && seats[1] == a);
        seats.seat({device(a), device(b)}, Mode::kMenu, 20);
        CHECK(seats[0] == b && seats[1] == a);
        seats.seat({device(a), device(b)}, Mode::kMenu, 2000);
        CHECK(!seats.takeSaved());
    }

    // One that never comes: P2 moves up as they stop, and is saved then.
    {
        Seats seats;
        seats.restore({{"p1", b.text()}, {"p2", a.text()}}, 1000);
        seats.seat({device(a)}, Mode::kMenu, 10);
        CHECK(!seats.takeSaved());
        seats.seat({device(a)}, Mode::kMenu, 2000);
        CHECK(seats[0] == a && seats[1].none());
        const auto saved = seats.takeSaved();
        CHECK(saved && saved->at("p1") == a.text() && !saved->count("p2"));
    }

    // An idle twin never takes a seat its model's device leaves, from the
    // poll it goes on; that device back on another path does.
    {
        Seats seats;
        const DeviceId twin = sdl("guid-a", "/dev/twin"), back = sdl("guid-a", "/dev/new");
        press(seats, {device(a, pad::kR2), device(twin)});
        seats.follow({device(twin)}, false);
        CHECK(seats.away(0) && seats[0] == a);
        seats.follow({device(twin)}, false);
        CHECK(seats.away(0) && seats[0] == a);
        seats.follow({device(twin), device(back)}, false);
        CHECK(!seats.away(0) && seats[0] == back);
    }

    // Red held on the Controllers screen to leave it takes no seat after.
    {
        Seats seats;
        press(seats, {device(a, pad::kR2), device(b)});
        seats.seat({device(a), device(b, pad::kCircle)}, Mode::kControllers, 0);
        seats.seat({device(a), device(b, pad::kCircle)}, Mode::kMenu, 0);
        CHECK(seats[0] == a);
    }

    // A seat's mode is its controller's as it sits: a gamepad's pad, a
    // guitar's guitar, none's the keyboard's guitar.
    {
        Seats seats;
        Read pad = device(a), guitar = device(b);
        pad.kind = DeviceKind::kGamepad;
        seats.setModes({pad, guitar, keyboard()}, false);
        CHECK(!seats.pad(0));
        pad.buttons = pad::kR2;
        press(seats, {pad, guitar});
        seats.setModes({pad, guitar}, false);
        CHECK(seats[0] == a && seats.pad(0));
        guitar.buttons = pad::kR2;
        press(seats, {device(a), guitar});
        seats.setModes({device(a), guitar}, false);
        CHECK(seats[0] == b && !seats.pad(0));
    }
    // Kept while it is away, and while a song is up whatever happens: the
    // controller's mode changed on the Controllers screen over a paused
    // song plays from the song's end.
    {
        Seats seats;
        Read pad = device(a, pad::kR2);
        pad.kind = DeviceKind::kGamepad;
        press(seats, {pad});
        pad.buttons = 0;
        seats.setModes({pad}, false);
        seats.follow({keyboard()}, false);
        seats.setModes({keyboard()}, false);
        CHECK(seats.away(0) && seats.pad(0));
        seats.follow({pad}, false);
        pad.kind = DeviceKind::kGuitar;
        seats.setModes({pad}, true);
        CHECK(seats.pad(0));
        seats.setModes({pad}, false);
        CHECK(!seats.pad(0));
    }

    // A scenario's devices are never saved.
    {
        Seats seats;
        const DeviceId scripted{DeviceId::kScripted, "guitar", ""};
        press(seats, {device(scripted, pad::kR2)});
        CHECK(seats[0] == scripted);
        CHECK(!seats.takeSaved());
    }

    // [seats] keeps each as text, read back the same.
    for (const DeviceId &id : {a, kKeyboard, DeviceId{DeviceId::kScripted, "pad", ""}, sdl("guid", "")})
        CHECK(DeviceId::parse(id.text()) == id);
    CHECK(DeviceId::parse("").none());

    if (s_failed)
    {
        std::cerr << s_failed << " failed" << std::endl;
        return 1;
    }
    std::cerr << "seats: all passed" << std::endl;
    return 0;
}
