#pragma once

// Quickplay's setlists: GH2's own, which the game builds from its campaign,
// and one per game added, its songs in its tiers, shown in its own song
// list scene and unlocked as GH2's are. Which is shown changes quickplay's
// list and nothing else.
//
//   {setlist select <name>}  TRUE if it exists and was not already shown
//   {setlist look}           the shown one's song list scene
//   {setlist shown}          the shown one's name
//   {setlist exists <name>}  TRUE if it was added
//
// GH2's is gh2.

#include "addresses.h"

#include <array>
#include <string>
#include <vector>

class PS2Runtime;

namespace gh2::setlists
{
    struct Tier
    {
        std::string venue;  // where its songs play when their own is locked
        std::string header; // headed by locale token song_header_<header>
        std::vector<std::string> songs;
        bool encore = true; // its last song is locked as its encore
    };

    // `look` is a milo path from the ARK root (ui/sel_song_quickplay.milo).
    // Its songs' high scores start on `scoreNames`, its game's own
    // highscore_dummy_0..4, and save under `name` (save/save.h).
    void add(const std::string &name, std::vector<Tier> tiers, const std::string &look,
             const std::array<std::string, 5> &scoreNames);

    void install(PS2Runtime &runtime, const Addresses &addresses);
}
