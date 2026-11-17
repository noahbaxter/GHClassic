#pragma once

// Each song won, alone in quickplay or a career or in a multiplayer mode,
// kept as each player's best (save/best.h) under the note engine played, by
// its game and name or as another module names it, as the profile each seat
// plays as (save/profiles.h).

#include "addresses.h"

#include <functional>
#include <optional>
#include <string>

class PS2Runtime;

namespace gh2::save
{
    // Songs another module knows better: its name in [best] and the part
    // played (custom songs by their chart, played as guitar or bass); none
    // for any other song.
    struct NamedSong
    {
        std::string song, part;
    };
    void nameSongs(std::function<std::optional<NamedSong>(const std::string &song)> namer);
    bool isNamed(const std::string &song);

    // [best]'s key for `song` played now on `difficulty` (easy .. expert),
    // as `part` where no namer says otherwise, on `controller`: gamepad for
    // a seat in pad mode (seatController), else guitar. At another speed than
    // 100% the key ends in it, ".s150", each speed's runs kept apart.
    std::string playedKey(const std::string &song, const std::string &difficulty, const std::string &part = "guitar",
                          const std::string &controller = "guitar");
    // The controller seat `seat` plays on, as playedKey takes it.
    std::string seatController(int seat);

    void installResults(PS2Runtime &runtime, const Addresses &addresses);
}
