#pragma once

// Quickplay's setlists, one per game: the game played's own
// (content/campaigns.h), which the game builds from its campaign, and every
// other's, its songs in its tiers, shown in its own song list scene and
// unlocked as the game's own are. Which is shown changes quickplay's list
// and nothing else.
//
//   {setlist select <name>}  TRUE if it exists and was not already shown
//   {setlist look}           the shown one's song list scene
//   {setlist shown}          the shown one's name
//
// Each is named for its game: gh2, gh80s.

#include "addresses.h"
#include "formats/dtb.h"
#include "milo/milo.h"

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
        // A career tier: its last song is locked as its encore, where its
        // game has them (content/encores.h).
        bool encore = true;
    };

    // campaign.dta's (required_songs ...), easy first: the songs a venue
    // asks for, and the last venue's own count (GH1 keeps one for all).
    using Required = std::vector<std::array<int, 2>>;
    Required required(const dtb::Node &campaign);

    // `look` is a milo path from the ARK root (ui/sel_song_quickplay.milo).
    // Its songs' high scores start on `scoreNames`, its game's own
    // highscore_dummy_0..4, and save under `name` (save/save.h). With
    // `required`, its career's, an encore opens as that career opens it.
    void add(const std::string &name, std::vector<Tier> tiers, const std::string &look,
             const std::array<std::string, 5> &scoreNames, Required required = {});

    // That game's song list scene, served at <game>/ui/sel_song_quickplay.milo
    // and turning with the menus as the game's own does.
    void addLook(const std::string &game, milo::Dir scene);

    // A game's career songs: its tiers' own, without the store's.
    std::vector<std::string> careerSongs(const std::string &name);

    // A game that keeps its career as GH2 does, read off its disc: the
    // career's tiers (campaign.dta's order), then the store's songs, in its
    // own scene, <game>/ui/sel_song_quickplay.milo. Its songs go into the
    // config and are served from that disc, unless they are `inConfig`
    // already, as the game disc's are.
    void addDisc(const std::string &game, size_t disc, bool inConfig);

    void install(PS2Runtime &runtime, const Addresses &addresses);
}
