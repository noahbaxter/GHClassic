#pragma once

// GH1's venue scripts as GH2's (gh1/scripts.cpp).

#include <cstddef>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace gh2::gh1
{
    // For each of a GH1 scene's anims, by GH1's name, the objects in GH2's
    // dir that stand for it: whatever drives the anim in GH1 drives these.
    using Drivers = std::map<std::string, std::vector<std::string>>;

    // The Group of an Environ's own children (gh1/venues.cpp), which a
    // script shows and hides in its place.
    inline std::string drawsOf(const std::string &of) { return of + ".draws"; }

    // The anims that GH1 venue's script drives itself.
    std::set<std::string> scripted(size_t disc, const std::string &gh1);

    // Those it sets going by switch_anim or switch_anim_rt, which take the
    // script's frame past their own filters (VenueAnim::Poll, GH1
    // 0x17cff0, sets their filtered frame). An anim_task's goes through
    // them, as any frame set (RndAnimatable::SetFrame, GH1 0x1b3d00).
    std::set<std::string> switched(size_t disc, const std::string &gh1);

    // Writes that GH1 venue's script into `layer` as handlers on the type
    // of the GH2 venue standing in for it, over the `objects` its dir holds.
    // `kit` is the room's drum kit, shown only for GH1's drummer
    // (content/band.h), and `spots` the waypoint on each walk spot its
    // guitarist stands on.
    void addScripts(size_t layer, size_t disc, const std::string &gh1, const std::string &gh2, const Drivers &drivers,
                    const std::set<std::string> &objects, const std::string &kit,
                    const std::vector<std::string> &spots);
}
