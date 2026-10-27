#pragma once

// Who sits where: which device each seat reads, as the presses of each poll
// move them. Only the host thread's poll (input.cpp) drives it; it reads no
// device and writes no file, so tests can run it.

#include "host/bindings.h"
#include "host/device_id.h"
#include "host/input.h"

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace gh2::input
{
    // What is up, which decides what a press does to the seats.
    enum class Mode
    {
        kMenu,
        kSong, // a song is up and holds the seats
    };
    inline bool inSong(Mode mode)
    {
        return mode == Mode::kSong;
    }

    // One device this poll.
    struct Read
    {
        DeviceId id;
        std::string name;  // as the log names it
        DeviceKind kind = DeviceKind::kController;
        std::string label; // what a player is shown
        // Each of its instrument's actions, 0 to 1; and the libpad buttons
        // they press, which is all the seats read, whatever the instrument.
        std::array<float, kActionCount> held{};
        uint16_t buttons = 0;
    };

    // A fret, pad, strum or Start: what claims a seat. Whammy, tilt and
    // star power never do, so a resting axis cannot.
    bool pressing(uint16_t buttons);

    class Seats
    {
    public:
        // input.ini's [seats]: who sat where last time, put back as those
        // devices show up until `untilMs`.
        void restore(std::map<std::string, std::string> saved, uint64_t untilMs);

        void seat(const std::vector<Read> &reads, Mode mode, uint64_t nowMs);
        // A seat's device gone or back. `readingNone` in a window without
        // the controllers, which pauses for none.
        void follow(const std::vector<Read> &reads, bool readingNone);
        // Whether a seated device still holds what seated it, playing
        // nothing; false once it lets go, for good.
        bool holding(const Read &r);

        const DeviceId &operator[](int seat) const { return m_seats[seat]; }
        int seatOf(const DeviceId &id) const;
        // A seated device not plugged in.
        bool away(int seat) const { return m_away[seat]; }
        // [seats] as they now are, once each time they change.
        std::optional<std::map<std::string, std::string>> takeSaved();

    private:
        // `joined`: by its Start, which the game never sees.
        void sit(int seat, const Read &r, bool joined);

        // The device each seat reads, by identity, so a replugged one finds
        // its seat; none for none.
        DeviceId m_seats[kSeats];
        // Each device's buttons last poll, so only one going down counts,
        // and those that play nothing until they let go of the Start that
        // seated them.
        std::map<DeviceId, uint16_t> m_held;
        std::set<DeviceId> m_letGo;
        std::map<std::string, std::string> m_saved; // as [seats] has it
        bool m_savedChanged = false;
        uint64_t m_restoreUntilMs = 0;
        bool m_away[kSeats] = {};
        // The devices there as each seat's went, none of which is it.
        std::vector<DeviceId> m_thereWhenAway[kSeats];
    };
}
