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

    // A device holding these libpad buttons.
    Read device(const DeviceId &id, uint16_t buttons = 0)
    {
        Read r{id, id.text(), DeviceKind::kGuitar, id.text()};
        r.buttons = buttons;
        return r;
    }

    // One menu poll, then one with everything let go, so the next press is
    // a new one.
    void press(Seats &seats, std::vector<Read> reads, Mode mode = Mode::kMenu)
    {
        seats.seat(reads, mode);
        for (Read &r : reads)
            r.buttons = 0;
        seats.seat(reads, mode);
    }
}

int main()
{
    const DeviceId a = sdl("guid-a", "/dev/a"), b = sdl("guid-b", "/dev/b");

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

    // Start that seated a device plays nothing until it is let go.
    {
        Seats seats;
        press(seats, {device(a, pad::kR2), device(b)});
        seats.seat({device(a), device(b, pad::kStart)}, Mode::kMenu);
        CHECK(seats[0] == b);
        CHECK(seats.holding(device(b, pad::kStart)));
        CHECK(!seats.holding(device(b)));
        CHECK(!seats.holding(device(b, pad::kStart)));
    }

    // In a song nothing seats, but a seat whose device is gone is taken by
    // Start on any device without one.
    {
        Seats seats;
        press(seats, {device(a, pad::kR2), device(b)});
        press(seats, {device(b, pad::kStart)}, Mode::kSong);
        CHECK(seats[0] == a);
        seats.follow({device(b)}, false);
        CHECK(seats.away(0));
        press(seats, {device(b, pad::kStart)}, Mode::kSong);
        CHECK(seats[0] == b);
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

    // A window without the controllers pauses for no one.
    {
        Seats seats;
        press(seats, {device(a, pad::kR2)});
        seats.follow({}, true);
        CHECK(!seats.away(0));
    }

    if (s_failed)
    {
        std::cerr << s_failed << " failed" << std::endl;
        return 1;
    }
    std::cerr << "seats: all passed" << std::endl;
    return 0;
}
