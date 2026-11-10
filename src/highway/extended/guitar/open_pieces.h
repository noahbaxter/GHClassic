#pragma once

// The pieces an open is drawn with (highway/extended/guitar/open_gems.h): its
// wide meshes, purple and cyan textures and caps' streaks, made in the track's
// scene as it is read.

#include "milo/milo.h"

#include <cstdint>
#include <string>
#include <vector>

namespace gh2::guitar
{
    // The wide mesh drawn for a part of the middle lane's, of a rep of
    // `type`, its gem in a star phrase or not, a tap or not.
    std::string openName(const std::string &mesh, uint32_t type, bool phrase, bool tap);
    // How high an open's cap stands over a lane gem's foot, a HOPO's or a
    // strummed one's: an open chord's frets are lifted that far onto it.
    float openCapTop(bool hopo);
    // The materials of the caps' streaks, scrolled as they are drawn.
    std::vector<std::string> openStreakMats();

    // The track's scene with the open gem's pieces (addToTrackScene's).
    void addOpenPieces(milo::Dir &scene);
}
