#pragma once

#include "formats/dtb.h"

#include <cstddef>

namespace gh2::gh1
{
    // GH1's band, its two singers, bassist, drummer and keyboardist, each a
    // character of its own beside GH2's of that name (content/band.h):
    // GH2's with GH1's skin, bones and rest (gh1/rig.h), playing GH2's clip
    // set with GH1's motion and graph. `macros` holds GH1's anim sets
    // (charsys.dta's).
    void addBand(size_t gh2Disc, const dtb::Macros &macros);
}
