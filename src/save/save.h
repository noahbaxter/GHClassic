#pragma once

#include "addresses.h"

#include <array>
#include <cstdint>
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

    // Another game's own save folder on a PCSX2 card. An import takes its
    // songs' high scores from it, and an export writes them into it, keeping
    // the rest of a save already there or else starting one of no progress.
    struct CardGame
    {
        std::string game;     // its sections' name, as addScoreSongs's
        std::string dir;      // config/mc.dta's base_dir: BASLUS-21224
        bool gh1Layout;       // GH1's save (save/gh1_stream.h), else GH2's, as the 80s keeps
        uint32_t dataSize;    // its save file, written whole
        std::vector<uint8_t> iconSys;
        std::string iconFile; // gh.icn
        std::vector<uint8_t> icon;
    };
    void addCardGame(CardGame game);

    // Read and write `path` in place of the user data directory's save.bin.
    void usePath(const std::string &path);
    // Load the saves on this PCSX2 card (.ps2) instead, making them save.bin:
    // GH2's whole, and every added game's high scores.
    void importCard(const std::string &path);
    // Write the saves to this PCSX2 card after the load and each save,
    // formatting a new card if there is none. settings.ini's export_card does
    // the same to GHClassic.ps2 beside save.bin.
    void exportCard(const std::string &path);

    void install(PS2Runtime &runtime, const Addresses &addresses);
}
