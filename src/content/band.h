#pragma once

// Which game's backing band plays a song.
//
// A song's band is its entry's (band ...) or config's default_band, GH2's
// characters by name, each loaded from char/<name>/og/<name>.milo into the
// venue's singer, bassist or drummer by what its name holds
// (GameConfig::OnChangeChars, 0x1289c8). A game with a band of its own has
// each member as a character beside GH2's, and that one loads instead when
// the band is that game's: the game being played, whose venues they are, or
// the one settings.ini's [game] band names.
//
//   {band from}          whose band plays: venue, gh2 or gh1
//   {band from <which>}  that one's from the next song on
//   {band room_kit}      TRUE if the drummer plays the room's own kit

#include "addresses.h"

#include <string>

class PS2Runtime;

namespace gh2::band
{
    // `game` has its own of GH2's `member` (metal_singer): the character
    // `own`, whose name holds what GH2's does and whose files the caller
    // serves. `home` is the one for that game's own venues where it is
    // another: a drummer without GH2's kit, where the room has the game's.
    void add(const std::string &game, const std::string &member, const std::string &own,
             const std::string &home = "");

    void install(PS2Runtime &runtime, const Addresses &addresses);
}
