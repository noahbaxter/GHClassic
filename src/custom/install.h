#pragma once

// The player's own songs (custom/catalog.h) as quickplay's custom setlist,
// a tier for each folder of songs, in GH2's song list scene: every one the
// last scan found from the start, and those this one finds as it finds
// them. A song is in the game's config while its row is near the list's
// window, and its chart and audio are made from its folder when the game
// first reads them.

namespace gh2
{
    void installCustomSongs();
}
