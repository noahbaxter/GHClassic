#pragma once

#include "addresses.h"

#include <array>
#include <cstdint>
#include <set>
#include <string>
#include <vector>

class PS2Runtime;
struct R5900Context;

namespace gh2::save
{
    // A game's songs, which the high score table holds whichever game is
    // played: each starts on that game's five names (its locale's
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

    // Any game's careers, as the save's sections hold them: the songs band
    // `slot` (from 0) has passed in `game`'s on each difficulty, easy first,
    // and the songs any band has unlocked there on any. A song is passed
    // once it has stars, and unlocked only as progress: a fresh campaign's
    // own open songs are in neither.
    std::array<std::set<std::string>, 4> passedSongs(const std::string &game, int slot);
    std::set<std::string> unlockedSongs(const std::string &game);
    // Whether any band has unlocked that item, of any kind, in `game`'s
    // career: an outfit or guitar bought.
    bool unlockedItem(const std::string &game, const std::string &item);

    // A campaign switch (content/campaigns.h), either side of the Campaign
    // being rebuilt. leaveGame puts the career as it stands into the save's
    // sections, in memory, where the next save writes it and the two above
    // read it. enterGame takes the new Campaign as `game`'s fresh one and
    // loads that game's career from its own sections.
    // The game last entered when the save was written, which the next boot
    // opens as; gh2 for a save that names none.
    std::string lastGame();
    void leaveGame(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void enterGame(const std::string &game, uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);

    void install(PS2Runtime &runtime, const Addresses &addresses);
}
