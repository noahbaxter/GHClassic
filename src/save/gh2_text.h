#pragma once

// GH2's progress as store sections. Only what differs from a fresh campaign
// is written, so a value missing from the file is the game's own default.
//
//   [gh2]                    default_name, career_beaten
//   [band 1]                 name, which the band slot shares across games
//   [band 1 gh2]             tutorials, difficulty, cash, <type>.<item>
//   [band 1 gh2 expert]      character, outfit, guitar, finish, <type>.<item>
//   [gh2 coop]               song.<song>
//   [gh2 scores expert]      <song>.<rank> = <score> <name>
//
// An item's value lists its flags (store, unlocked, passed, or locked) and,
// for a song, score=, stars=, gold and trickle=.

#include "save/gh2_stream.h"
#include "save/store.h"

namespace gh2::save::gh2
{
    // Replaces every GH2 section in store with save's differences from fresh.
    void toStore(const Save &save, const Save &fresh, Store &store);
    // fresh with store's GH2 sections laid over it.
    Save fromStore(const Store &store, const Save &fresh);
}
