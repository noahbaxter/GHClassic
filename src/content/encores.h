#pragma once

// Careers without encores. GH2 keeps a venue's last song as its encore on
// every difficulty but easy; a game with none has all of a venue's songs
// open and passes the venue on its count of songs alone, as GH2 does on
// easy (CampaignState::CheckUnlockVenue, 0x1322c8).

#include "addresses.h"

#include <string>

class PS2Runtime;

namespace gh2::encores
{
    // That game's career has none.
    void none(const std::string &game);
    bool has(const std::string &game);

    void install(PS2Runtime &runtime, const Addresses &addresses);
}
