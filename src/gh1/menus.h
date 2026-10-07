#pragma once

#include <cstddef>
#include <map>
#include <set>
#include <string>

namespace gh2::gh1
{
    // GH1's menu scenes from that disc in that layer, each in place of GH2's
    // scene of the same name, for GH2's script of that screen to drive.
    // `text` gets the strings their labels and buttons name, by token.
    // Gives GH2's names of the scenes that are there.
    std::set<std::string> addMenus(size_t layer, size_t disc, std::map<std::string, std::string> &text);
}
