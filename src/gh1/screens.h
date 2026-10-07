#pragma once

#include "gh1/screen.h"

#include <vector>

namespace gh2::gh1
{
    // Each of GH1's menu scenes that shows in its campaign, as the scene of
    // GH2's it stands in for.
    const std::vector<Screen> &screens();
}
