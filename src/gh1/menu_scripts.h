#pragma once

#include "formats/dtb.h"

#include <set>
#include <string>

namespace gh2::gh1
{
    // One of GH2's UI scripts as GH1's campaign runs it: its screens'
    // handlers as gh1/menus.dta has them, for those whose scenes (by GH2's
    // names) are among `scenes`. False if it has none of those.
    bool fitScript(dtb::Node &file, const std::set<std::string> &scenes);
}
