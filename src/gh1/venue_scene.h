#pragma once

// A GH1 venue's scene as GH2 draws it (gh1/venues.cpp).

#include "gh1/scripts.h"
#include "milo/milo.h"

#include <set>
#include <string>
#include <vector>

namespace gh2::gh1
{
    // An object made for GH2's dir.
    struct Object
    {
        std::string cls, name;
        milo::Bytes body;
        // A Mesh no View of GH1's tree reaches: GH1 never draws it.
        bool unreached = false;
    };

    // The objects of `scene` in those classes, less the names in
    // `without`, drawn from `top` but for the names in `hidden`. An anim
    // in `scripted` is left out of every View's, for the script that
    // drives it. `drivers` gets what stands for each anim.
    std::vector<Object> objects(const milo::Dir &scene, const std::set<std::string> &classes, const std::string &top,
                                const std::set<std::string> &without, const std::set<std::string> &hidden,
                                const std::set<std::string> &scripted, Drivers &drivers);
}
