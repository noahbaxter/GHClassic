#pragma once

// The quickplay pre-show's venue posters: a show poster for each venue,
// made on the host from the art on the player's discs. Each is a window of
// its game's venue map (GH2's and the 80s' sel_venue pages, GH1's map
// bitmaps) round the venue's own drawing and ribbon, as venue select lays
// them, in the map's own paper edge, lettered TONIGHT!! / LIVE AT <name> in
// the traced faces (fonts/rokk, fonts/helveticablackcondensed) and taped up
// with the 360 venue select's corner tape. Below Nilbog High the map's last
// page is cut off by its paper; its road and field are carried on there
// from its own art and the Stonehenge page's.

#include "render/scene.h"

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace gh2::venue_posters
{
    // A poster's size, tape margin and all: its alpha clear round the paper.
    constexpr int kWidth = 460, kHeight = 691;

    struct Venue
    {
        const char *venue; // {campaigns venues}
        const char *art;   // its art's name
        const char *title;
    };

    // Each venue of `game` that has a poster, not counting "unknown".
    const std::vector<Venue> &venues(const std::string &game);

    // The poster for `game` (gh2, gh80s, gh1) and `venue` ({campaigns
    // venues}, GH1's by the GH2 venue standing in for it; or "unknown", for
    // song defaults: blank paper with a big ? on it), made by prepare, or
    // the first time any of that game's is asked for. None when that game
    // is not mounted or its art cannot be read. Any thread.
    std::shared_ptr<const TextureData> poster(const std::string &game, const std::string &venue);

    // Every game's posters made now, on a thread of their own: a few
    // seconds' work, done long before the hub draws one. After
    // ark::install.
    void prepare();
}
