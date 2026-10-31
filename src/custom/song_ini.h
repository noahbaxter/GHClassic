#pragma once

// song.ini, the [song] section's tags (name, artist, delay, hopo_frequency
// ...), keys lowercased. A .sng carries the same tags as its metadata.

#include "custom/chart.h"

#include <map>
#include <optional>
#include <string>

namespace gh2::custom
{
    using Tags = std::map<std::string, std::string>;

    Tags parseIni(const std::string &text);

    std::optional<std::string> tag(const Tags &tags, const std::string &key);
    std::optional<long> number(const Tags &tags, const std::string &key);

    // How the chart reads, from the tags.
    Options options(const Tags &tags);
}
