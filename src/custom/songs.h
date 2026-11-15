#pragma once

// The custom songs as the game has them now: the catalog every list, file
// and command reads (custom/catalog.h), the instrument quickplay plays, and
// so the part each song plays.

#include "custom/catalog.h"

#include <cstdint>
#include <string>

namespace gh2::custom
{
    // Never destroyed: the scan outlives main's statics.
    Catalog &catalog();

    // The instrument quickplay plays, guitar or bass: a song without it
    // plays the part it has.
    void setInstrument(Part part);
    const char *instrumentName();
    // It and the list's sort, kept from run to run in state.ini, [custom].
    void keepListing();

    // A song's lead guitar part, else its bass.
    Part playedPart(uint8_t parts);
    // The part it plays now, as the instrument picked says.
    Part played(const Song &song);
    // A part's stem's name.
    std::string playedStem(Part part);
    // A song's files for that part, songs/<id>/<id>: its own part's there,
    // the bass in place of another part's at <id>_bass.
    std::string base(const std::string &id, uint8_t parts, Part part);
}
