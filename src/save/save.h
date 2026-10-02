#pragma once

#include "addresses.h"

#include <array>
#include <string>
#include <vector>

class PS2Runtime;

namespace gh2::save
{
    // Another game's songs, which GH2's high score table holds beside its
    // own: each starts on that game's five names (its locale's
    // highscore_dummy_0..4), and their scores go in [<game> scores ...].
    // Before the game boots.
    void addScoreSongs(const std::string &game, const std::vector<std::string> &songs,
                       const std::array<std::string, 5> &names);

    // Read and write `path` in place of the user data directory's save.bin.
    void usePath(const std::string &path);
    // Load GH2's save from this PCSX2 card (.ps2) instead, making it save.bin.
    void importCard(const std::string &path);
    // Write GH2's save to this PCSX2 card after the load and each save,
    // formatting a new card if there is none. settings.ini's export_card does
    // the same to GHClassic.ps2 beside save.bin.
    void exportCard(const std::string &path);

    void install(PS2Runtime &runtime, const Addresses &addresses);
}
