#pragma once

#include <cstddef>

namespace gh2::gh1
{
    // GH1's career and bonus songs as quickplay's gh1 setlist: each entry
    // and chart in GH2's form, the audio read off GH1's disc as it is.
    void addSetlist(size_t disc, size_t gh2Disc);
}
