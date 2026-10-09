#pragma once

// GH1's crowd in GH2's venues (gh1/venues.cpp).

#include "gh1/cameras.h"
#include "milo/milo.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace gh2::gh1
{
    // Each Crowd<nn>.mm's places: a count, then that many transforms.
    std::vector<milo::Bytes> crowdPlaces(const std::vector<const milo::Dir *> &scenes);

    // The crowd members GH1 draws whole in each of a venue's regions, of
    // those `places`: those a card of that `height` stands in, `whole` of
    // them at most.
    std::vector<Region> crowdRegions(const std::vector<const milo::Dir *> &scenes,
                                     const std::vector<milo::Bytes> &places, float height, size_t whole);

    // GH2's chars dir with GH1's flat crowd, those `places` with cards of
    // that `height`, for the stand-in's. Returns the crowd's stamp.
    uint32_t crowd(milo::Dir &chars, const std::vector<milo::Bytes> &places, float height);
}
