#pragma once

// A career GH2's code keeps, as store sections named for its game: gh2's
// own below, and gh80s's the same under its name (content/campaigns.h). Only
// what differs from a fresh campaign is written, so a value missing from the
// file is the game's own default.
//
//   [gh2]                    career_beaten
//   [classic]                default_name, the high score name every game offers
//   [band 1]                 name, which the band slot shares across games
//   [band 1 gh2]             tutorials, difficulty, cash, <type>.<item>
//   [band 1 gh2 expert]      character, outfit, guitar, finish, <type>.<item>
//   [gh2 coop]               song.<song>
//   [gh2 scores expert]      <song>.<rank> = <score> <name>
//
// An item's value lists its flags (store, unlocked, passed, or locked) and,
// for a song, score=, stars=, gold and trickle=. A song another game brings
// keeps its scores in that game's sections: [gh1 scores expert].

#include "save/gh2_stream.h"
#include "save/store.h"

#include <map>
#include <string>

namespace gh2::save::gh2
{
    // Song to the game it came from.
    using SongGames = std::map<std::string, std::string>;

    // The game whose career the save is, which names its sections (gh2
    // above), and each song's game; songs missing are that game's.
    struct Games
    {
        std::string own = "gh2";
        SongGames songs;
    };

    // Replaces every section of that game's in store with save's differences
    // from fresh. A band the save has none of loses its sections for every
    // game: the slot is shared, and its next band starts from nothing.
    void toStore(const Save &save, const Save &fresh, Store &store, const Games &games = {});
    // fresh with store's sections of that game's laid over it.
    Save fromStore(const Store &store, const Save &fresh, const Games &games = {});
}
