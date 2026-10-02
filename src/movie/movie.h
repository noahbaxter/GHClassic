#pragma once

#include "addresses.h"

class PS2Runtime;

namespace gh2
{
    void installMovies(PS2Runtime &runtime, const Addresses &addresses);

    // PlayMovie is running, holding the game thread until the movie ends.
    bool moviePlaying();
}
