#pragma once

#include <cstdint>

namespace gh2
{
    // Where each hooked function lives in one executable. Every hook takes
    // its address from here, so a second executable is a second table.
    struct Addresses
    {
        uint32_t entry;
        uint32_t psRndFlushPacket;
        uint32_t playMovie;
    };

    // Guitar Hero II (USA), SLUS-21447.
    inline constexpr Addresses kSlus21447{
        .entry = 0x100bf0u,
        .psRndFlushPacket = 0x3d7d58u,
        .playMovie = 0x21bb60u,
    };
}
