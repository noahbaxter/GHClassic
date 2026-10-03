#pragma once

// The games GH Classic draws content from: gh2 (the game disc, always
// there), gh1, gh80s and gh2x (360 GH2), each mounted when a disc of any of
// its supported releases is (config/executables.toml's [[release]] game and
// role). Anything a game adds, its songs, outfits, venues or career, goes in
// only when it is mounted, and anything that offers it asks here first.
//
//   {mounted <game>}  TRUE if that game's disc is mounted

#include "addresses.h"

#include <cstddef>
#include <optional>
#include <string>

class PS2Runtime;

namespace gh2::games
{
    // Its disc's index in the archive (ark::readFile), once ark::install has
    // run, or none when not mounted.
    std::optional<size_t> disc(const std::string &game);

    bool mounted(const std::string &game);

    void install(PS2Runtime &runtime, const Addresses &addresses);
}
