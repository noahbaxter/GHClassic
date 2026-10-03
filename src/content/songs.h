#pragma once

// Songs beyond config's own (songs ...), each an entry as songs.dta has it,
// for the game to find by name: its audio, chart and quickplay setup.
// Joining the config lists a song nowhere; setlists do that.

#include <string>

namespace gh2::songs
{
    // One entry's script text: (name (name "...") (song ...) ...).
    void add(const std::string &entry);

    // After every add.
    void install();
}
