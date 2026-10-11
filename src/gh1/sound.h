#pragma once

// GH1's sounds where GH2's differ.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace gh2::gh1
{
    // GH1's credits music onto `layer`, and GH2's in-game bank with GH1's
    // win stingers for every game.
    void addSounds(size_t layer, size_t disc);

    // A bank as `bank` but every sample silent.
    std::optional<std::vector<uint8_t>> silenced(const std::vector<uint8_t> &bank);
}
