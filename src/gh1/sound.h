#pragma once

// GH1's sounds where GH2's differ.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace gh2::gh1
{
    // GH2's in-game bank with GH1's own star power and win stingers, and
    // silent where GH2 has a sound GH1 does not, and GH1's credits music,
    // onto `layer`.
    void addSounds(size_t layer, size_t disc);

    // The GH2 venue standing in for GH1's `venue`'s bank (`standIn`) with
    // GH1's own recordings from that venue's .bnk and .nse on `disc`. They
    // are GH2's, re-encoded and some cut to 16 kHz or trimmed, under the
    // same sounds layered the same way, so each of GH2's samples takes the
    // GH1 sample that the same sounds play. One of a sound GH1 has not (a
    // blown big note, the city's engine) is silent. None if GH1's bank
    // cannot be read.
    std::optional<std::vector<uint8_t>> venueBank(const std::vector<uint8_t> &standIn, size_t disc,
                                                  const std::string &venue);
}
