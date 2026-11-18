#pragma once

// The custom list's printout as pieces of the song list scene: the
// multiplayer stats' green-bar paper, the store's barcode sticker, the
// song's album art over the sticker's picture and tape holding it up, hung
// up by ui/dta/quickplay.dta.

#include "milo/milo.h"

#include <cstdint>

namespace gh2::setlist_printout
{
    inline constexpr uint32_t kArtStandIn = 16u; // the least a stand-in is

    // Each piece added to a scene of GH2's paper, out of sight; none where
    // the scene or the discs' pieces are not GH2's.
    void add(milo::Dir &scene);
}
