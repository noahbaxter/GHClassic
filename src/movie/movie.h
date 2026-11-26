#pragma once

#include "addresses.h"

#include <string>

class PS2Runtime;

namespace gh2
{
    void installMovies(PS2Runtime &runtime, const Addresses &addresses);

    // PlayMovie is running, holding the game thread until the movie ends.
    bool moviePlaying();

    // A movie the player would find ("videos/hmx.pss"): Europe's GH2 disc
    // has none of the bonus videos its store still names.
    bool movieExists(const std::string &path);
}
