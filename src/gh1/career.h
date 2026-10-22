#pragma once

#include "gh1/guitarist.h"

#include <cstddef>
#include <vector>

namespace gh2::gh1
{
    // GH1's career as the gh1 campaign, played by those guitarists, each
    // built already. After its setlist.
    void addCareer(size_t disc, const std::vector<Guitarist> &guitarists);

    // GH1's highway and HUD: each of GH2's track and HUD scenes but co-op's,
    // with GH1's art where GH1 has the same, made as a song first asks for
    // it while the song's highway is GH1's (campaigns::gh1Highway), from the
    // game disc's as the game gets it.
    // After every other maker or fix of those (the streak counter's, the
    // fonts', the 360's), so it starts from them and is served over them.
    void addHighway(size_t disc);
}
