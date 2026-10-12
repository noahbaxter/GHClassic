#pragma once

#include <cstddef>

namespace gh2::gh1
{
    // GH1's venues in that layer, each in place of the GH2 venue standing
    // in for it (gh1/songs.h): GH1's rooms from that disc.
    void addVenues(size_t layer, size_t disc);
}
