#pragma once

// Player builds have no autoplay. GH2's own (a player config's autoplay, the
// mode's force_autoplay, the player matcher's handler) all set it through
// BeatMatcher::SetAutoplay, which is held to off. Local builds keep it, for
// scenarios (GHC_DEV_TOOLS in CMakeLists.txt).

#include "addresses.h"

class PS2Runtime;

namespace gh2
{
    void installNoAutoplay(PS2Runtime &runtime, const Addresses &addresses);
}
