#pragma once

#include "milo/milo.h"

#include <cstddef>
#include <string>
#include <vector>

namespace gh2::gh1
{
    // Where GH1's band stands in a venue, for its camera shots.
    struct Stage
    {
        // stage_spot_01.mesh's world, which GH1's fixed shots look at.
        milo::Bytes spot;
        // The walk waypoints on each of GH1's walk spots, by name.
        std::vector<std::vector<std::string>> walks;
    };

    // GH1's camera shots for its venue `gh1` in that layer, as the CamShots
    // of the GH2 venue `gh2` standing in for it: its world dirs for one
    // player, co-op and versus, less GH2's own shots.
    void addCameras(size_t layer, size_t disc, const std::string &gh1, const std::string &gh2, const Stage &stage);
}
