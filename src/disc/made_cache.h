#pragma once

// Content made from the discs' own assets, kept by the run that first makes
// it and read back by every run after: nothing made from a disc ships with
// the game, and nothing is made twice. One file a name, in the user's data
// directory's cache/. One kept by another build, or with other discs
// mounted, is not read: it is made again and kept over the old one.

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace gh2::made_cache
{
    // `name`'s bytes, if this build kept them with these discs. Any thread,
    // once ark::install has run.
    std::optional<std::vector<uint8_t>> read(const std::string &name);

    // Kept as `name` for the runs after. Any thread, once ark::install has
    // run.
    void write(const std::string &name, const std::vector<uint8_t> &bytes);
}
