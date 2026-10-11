#pragma once

// Text beyond the locale's own, for tokens the host adds, and each game's
// text where it belongs outside a career.
//
// Every game's table names the same things by the same tokens: the 80s'
// "fest" is its Rock for Safety Tour where GH2's is the Vans Warped Tour, and
// GH1's rooms and guitarists are under GH2's venues' and characters' names.
// Outside a career the table is GH2's, and a token whose text another game
// has its own of takes its owner's: a venue's name its venue's game's, a
// character's name and blurbs its outfit's game's, the rest its screen's
// (the theme's menus, the in-song look's screens around a song).

#include "addresses.h"

#include <cstdint>
#include <set>
#include <string>

class PS2Runtime;
struct R5900Context;

namespace gh2::locale
{
    // `token` localizes to `text`.
    void add(const std::string &token, const std::string &text);

    // The Locale's table (its first word, 0x51f1b8) becomes `game`'s: the
    // locale's files as the archive gives them then, read the first time
    // (Locale::Init, 0x2cb798) with every added token in, and kept outside
    // the game's heap (script::park). No kept table is let go, so text the
    // game still points at stays.
    void enter(const std::string &game, uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    // `game`'s table read and kept the same way, the Locale's staying as it
    // is: the caller puts that game's files in front first.
    void build(const std::string &game, uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);

    enum Owner
    {
        kScreen,    // any token not another's
        kVenue,     // the venues' names
        kCharacter, // the characters' names and blurbs
        kOwners,
    };
    // Whose text that owner's tokens take from here on.
    void own(Owner owner, const std::string &game);
    // The tokens that are that owner's.
    void ownTokens(Owner owner, const std::set<std::string> &tokens);
    // The text `token` takes from its owner's table, where the table in the
    // Locale is GH2's and the owner's text differs from it; 0 otherwise.
    uint32_t owned(const char *token);

    // After every add.
    void install(PS2Runtime &runtime, const Addresses &addresses);
}
