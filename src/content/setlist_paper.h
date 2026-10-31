#pragma once

// The song list's paper going on as far down as a setlist does: GH2's
// sheets, wall and doodles put on again ahead of the view as it slides
// past them (content/setlists.h's frame and paper ops).

#include "milo/milo.h"
#include "script.h"

#include <optional>
#include <string>
#include <vector>

namespace gh2::setlist_paper
{
    // The slide made endless where the scene is GH2's paper.
    void endlessPaper(milo::Dir &scene);

    // {setlist frame|paper_base|paper_pose|paper ...} for the list whose
    // scene is `look`, its window's rows from `first` of every row's
    // `tiers`; none for another op.
    std::optional<script::Node> command(const script::Call &call, const std::string &look,
                                        const std::vector<size_t> &tiers, size_t first);
}
