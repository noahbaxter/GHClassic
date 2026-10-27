#include "host/seats.h"

#include <algorithm>
#include <iostream>
#include <iterator>
#include <utility>

namespace gh2::input
{
    namespace
    {
        constexpr uint16_t kFrets = pad::kR2 | pad::kCircle | pad::kTriangle | pad::kCross | pad::kSquare;
        constexpr uint16_t kClaims = kFrets | pad::kUp | pad::kDown | pad::kStart;
    }

    bool pressing(uint16_t buttons)
    {
        return (buttons & kClaims) != 0;
    }

    void Seats::restore(std::map<std::string, std::string> saved, uint64_t untilMs)
    {
        m_saved = std::move(saved);
        m_restoreUntilMs = untilMs;
    }

    int Seats::seatOf(const DeviceId &id) const
    {
        for (int seat = 0; seat < kSeats; ++seat)
            if (m_seats[seat] == id)
                return seat;
        return -1;
    }

    void Seats::sit(int seat, const Read &r, bool joined)
    {
        m_seats[seat] = r.id;
        if (joined)
            m_letGo.insert(r.id);
        std::cerr << "[input] P" << seat + 1 << ": " << r.name << ", " << kindName(r.kind) << std::endl;
    }

    void Seats::trade(int seat, int with, const Read &r)
    {
        std::swap(m_seats[seat], m_seats[with]);
        if (seat == 0 || with == 0)
            ++m_trades;
        std::cerr << "[input] " << r.name << " is P" << with + 1 << std::endl;
    }

    // With no seated device here, the first to press anything is P1, so one
    // player never has to join. While P1 plays alone, any fret, strum or
    // Start on another device takes P1 from whatever had it: the one player
    // picking up another controller. The keyboard's strums and Start are its
    // menu keys, which move P1's menus whoever holds it, so only its frets
    // take P1 that way. Otherwise only Start on a device with no seat does
    // anything to the seats: on Multiplayer's screen it joins, into the first
    // free seat, and anywhere else it takes P1. There, too, yellow moves a
    // seated player up a seat, the first round to the last. Not off a press a
    // seated device made in the same poll: Steam Input's mirror of a pad
    // presses with it. P1 is never empty while P2 is not.
    void Seats::seat(const std::vector<Read> &reads, Mode mode, uint64_t nowMs)
    {
        const bool playersStep = mode == Mode::kPlayers;
        // Only devices here are remembered, and a seat's while it is away.
        const auto here = [&](const DeviceId &id) {
            return std::any_of(reads.begin(), reads.end(), [&](const Read &r) { return r.id == id; });
        };
        std::erase_if(m_held, [&](const auto &held) { return !here(held.first); });
        std::erase_if(m_letGo, [&](const DeviceId &id) { return !here(id) && seatOf(id) < 0; });
        // Each button going down this poll.
        std::vector<uint16_t> down(reads.size());
        std::vector<bool> pressed(reads.size()), started(reads.size()), switched(reads.size());
        for (size_t i = 0; i < reads.size(); ++i)
        {
            const Read &r = reads[i];
            uint16_t &was = m_held[r.id];
            down[i] = static_cast<uint16_t>(r.buttons & ~was);
            was = r.buttons;
            pressed[i] = pressing(down[i]);
            started[i] = r.keyboard() ? r.tab : (down[i] & pad::kStart) != 0;
            switched[i] = r.keyboard() ? (down[i] & kFrets) != 0 : pressed[i];
        }
        // In a song a seat whose device is gone, paused for it, is taken by
        // Start (the keyboard's Esc too) on any without a seat: a pad that
        // comes back on another cable or by Bluetooth has another GUID, and
        // would never be found.
        if (inSong(mode))
        {
            for (int seat = 0; seat < kPlayers; ++seat)
                if (m_away[seat])
                    for (size_t i = 0; i < reads.size(); ++i)
                        if (seatOf(reads[i].id) < 0 && (started[i] || reads[i].escape))
                        {
                            sit(seat, reads[i], true);
                            break;
                        }
            return;
        }

        // Each seat's own device first, then one of its model, so two the
        // same are not swapped by the order SDL lists them in.
        if (nowMs < m_restoreUntilMs)
            for (bool byModel : {false, true})
                for (int seat = 0; seat < kPlayers; ++seat)
                {
                    const auto saved = m_saved.find("p" + std::to_string(seat + 1));
                    if (saved == m_saved.end() || !m_seats[seat].none())
                        continue;
                    const DeviceId id = DeviceId::parse(saved->second);
                    for (const Read &r : reads)
                        if (seatOf(r.id) < 0 && (byModel ? r.id.sameModel(id) : r.id == id))
                        {
                            sit(seat, r, false);
                            break;
                        }
                }

        const bool solo = m_seats[1].none();
        bool anyHere = false, seatedStart = false, seatedPress = false;
        for (size_t i = 0; i < reads.size(); ++i)
            if (seatOf(reads[i].id) >= 0)
            {
                anyHere = true;
                seatedStart = seatedStart || started[i];
                seatedPress = seatedPress || pressed[i];
            }
        if (!anyHere)
        {
            for (size_t i = 0; i < reads.size(); ++i)
                if (pressed[i])
                {
                    std::fill(std::begin(m_seats), std::end(m_seats), DeviceId{});
                    sit(0, reads[i], false);
                    break;
                }
        }
        else
        {
            for (size_t i = 0; i < reads.size(); ++i)
            {
                if (seatOf(reads[i].id) >= 0)
                    continue;
                int free = -1;
                for (int seat = 0; seat < kPlayers && free < 0; ++seat)
                    if (m_seats[seat].none())
                        free = seat;
                if (started[i] && !seatedStart && playersStep && free >= 0)
                    sit(free, reads[i], true);
                else if (started[i] && !seatedStart && !playersStep)
                    sit(0, reads[i], true);
                // One player: whatever they last pressed is theirs, the press
                // playing as it would on it.
                else if (solo && !playersStep && switched[i] && !seatedPress)
                    sit(0, reads[i], false);
            }
        }
        if (playersStep)
        {
            int seated = 0;
            while (seated < kPlayers && !m_seats[seated].none())
                ++seated;
            for (size_t i = 0; i < reads.size(); ++i)
            {
                const int seat = seatOf(reads[i].id);
                if (seat >= 0 && seated > 1 && (down[i] & pad::kTriangle))
                    trade(seat, (seat + seated - 1) % seated, reads[i]);
            }
        }
        for (int seat = 0; seat + 1 < kSeats; ++seat)
            if (m_seats[seat].none() && !m_seats[seat + 1].none())
            {
                std::swap(m_seats[seat], m_seats[seat + 1]);
                if (seat == 0)
                    ++m_trades;
                std::cerr << "[input] P" << seat + 2 << " is P" << seat + 1 << std::endl;
            }

        // A scenario's devices are not the player's to remember.
        std::map<std::string, std::string> now;
        for (int seat = 0; seat < kSeats; ++seat)
            if (!m_seats[seat].none() && m_seats[seat].backend != DeviceId::kScripted)
                now["p" + std::to_string(seat + 1)] = m_seats[seat].text();
        if (!now.empty() && now != m_saved)
        {
            m_saved = now;
            m_savedChanged = true;
        }
    }

    // A seat's device back from a replug: the same identity, or on Linux,
    // where its /dev/input node can change, one of its model in no other seat
    // and not there when it went: an idle twin is not it.
    void Seats::follow(const std::vector<Read> &reads, bool readingNone)
    {
        for (int seat = 0; seat < kSeats; ++seat)
        {
            const DeviceId id = m_seats[seat];
            bool here = id.none() || readingNone;
            for (const Read &r : reads)
                here = here || r.id == id;
            const std::vector<DeviceId> &there = m_thereWhenAway[seat];
            for (const Read &r : reads)
            {
                if (here || !r.id.sameModel(id) ||
                    std::find(std::begin(m_seats), std::end(m_seats), r.id) != std::end(m_seats) ||
                    std::find(there.begin(), there.end(), r.id) != there.end())
                    continue;
                m_seats[seat] = r.id;
                here = true;
                std::cerr << "[input] P" << seat + 1 << " back: " << r.name << std::endl;
            }
            if (m_away[seat] != !here)
            {
                m_away[seat] = !here;
                std::cerr << "[input] P" << seat + 1 << (here ? " plugged back in" : " unplugged") << std::endl;
                m_thereWhenAway[seat].clear();
                if (!here)
                    for (const Read &r : reads)
                        m_thereWhenAway[seat].push_back(r.id);
            }
        }
    }

    bool Seats::holding(const Read &r)
    {
        if (m_letGo.count(r.id) && pressing(r.buttons))
            return true;
        m_letGo.erase(r.id);
        return false;
    }

    std::optional<std::map<std::string, std::string>> Seats::takeSaved()
    {
        if (!m_savedChanged)
            return std::nullopt;
        m_savedChanged = false;
        return m_saved;
    }
}
