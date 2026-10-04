#pragma once

#include "gh1/guitarist.h"

#include <cstddef>
#include <vector>

namespace gh2::gh1
{
    // GH1's career as the gh1 campaign, played by those guitarists, each
    // built already. After its setlist.
    void addCareer(size_t disc, const std::vector<Guitarist> &guitarists);
}
