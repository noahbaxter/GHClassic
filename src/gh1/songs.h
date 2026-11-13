#pragma once

#include "formats/dtb.h"

#include <cstddef>
#include <string>

namespace gh2::gh1
{
    // GH1's two songs that nothing on its disc lists (songs/advharmony and
    // songs/graveyardshift), added to its songs.dta and to its store.dta's
    // songs, either of which may be null: sold as its bonus songs are.
    void addUnlistedSongs(dtb::Node *songs, dtb::Node *store);

    // GH1's career and bonus songs as quickplay's gh1 setlist: each entry
    // and chart in GH2's form, the audio read off GH1's disc as it is.
    void addSetlist(size_t disc, size_t gh2Disc);

    // That venue of GH1's by our name ("gh1_basement", content/sources.h),
    // the small club's for one GH1 has none of.
    std::string venue(const std::string &gh1);
    // GH2's venue standing in for it, whose files it is made from
    // (gh1/venues.h).
    std::string slot(const std::string &gh1);
}
