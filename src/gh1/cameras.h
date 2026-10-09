#pragma once

#include "milo/milo.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace gh2::gh1
{
    // The Views GH1 draws a venue's room and its lighting scene from
    // (venues/<v>/<v>.rnd and lighting.rnd).
    inline constexpr const char *kRoomView = "venue.view";
    inline constexpr const char *kLightingView = "lighting.view";

    // A crowd region: the crowd members drawn whole in it, each the crowd
    // archetype it is of and which of that one's places, and the sphere
    // about them (gh1/venues.cpp).
    struct Region
    {
        std::vector<std::pair<uint32_t, uint32_t>> members;
        float centre[3] = {0.0f, 0.0f, 0.0f};
        float radius = 0.0f;
    };

    // Where GH1's band stands in a venue, for its camera shots.
    struct Stage
    {
        // stage_spot_01.mesh's world, which GH1's fixed shots look at.
        milo::Bytes spot;
        // The waypoint on each walk spot GH1's guitarist stands on, by name.
        std::vector<std::string> walks;
        std::vector<Region> regions;
        // The crowd's stamp, which a shot naming members of it has to have.
        uint32_t crowdStamp = 0xffffffffu;
    };

    // GH1's camera shots for its venue `gh1` in that layer, as the CamShots
    // of the GH2 venue `gh2` standing in for it: its world dirs for one
    // player, co-op and versus, less GH2's own shots.
    void addCameras(size_t layer, size_t disc, const std::string &gh1, const std::string &gh2, const Stage &stage);
}
