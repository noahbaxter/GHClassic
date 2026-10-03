#pragma once

// Harmonix's stream ciphers: a file encrypted whole after a 4-byte seed,
// each byte XORed with the next of a keystream. PS2 GH1, GH2 and 80s scripts
// use the Rand table stream; 360 GH2's MAIN.HDR and scripts the older
// Park-Miller one.

#include <cstdint>
#include <vector>

namespace gh2::crypt
{
    using Bytes = std::vector<uint8_t>;

    Bytes randStream(const Bytes &file);
    Bytes parkMiller(const Bytes &file);
}
