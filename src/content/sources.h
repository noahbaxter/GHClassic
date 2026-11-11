#pragma once

// A source is anything content comes from: a game's disc, a romhack, a
// folder of customs. Every song of one goes by <source>_<name> everywhere
// past the disc, GH2's own included ("gh2_surrender"), so no two sources can
// share a name and nothing is anyone's by default.
//
// A GH2-format disc's files keep their names on the disc; addDisc serves
// them under ours. Its config scripts are changed as read (ark::addFix), and
// songs/<ours>/ is its songs/<name>/.

#include "formats/dtb.h"

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace gh2::sources
{
    // "gh2", "surrender" -> "gh2_surrender".
    inline std::string song(const std::string &source, const std::string &name)
    {
        return source + "_" + name;
    }

    // The name on its source's disc, when `ours` is that source's.
    inline std::optional<std::string> bare(const std::string &source, const std::string &ours)
    {
        const std::string prefix = source + "_";
        if (ours.size() <= prefix.size() || ours.compare(0u, prefix.size(), prefix) != 0)
            return std::nullopt;
        return ours.substr(prefix.size());
    }

    // That disc's songs (config/songs.dta, and `unlisted`, songs on it that
    // no entry names) under that source's names, in every script its retail
    // form names them in. Once ark::install has run, before anything reads
    // its config.
    void addDisc(const std::string &source, size_t disc, const std::vector<std::string> &unlisted = {});

    // One songs.dta entry of that source's as ours: its name, and its paths
    // under songs/<name>/.
    void entry(dtb::Node &entry, const std::string &source);
}
