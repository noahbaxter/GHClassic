#pragma once

// song's stream that much slower. "ghc_speed/<percent>/<a song's .vgs>" is

#include "addresses.h"

class PS2Runtime;

namespace gh2::song_speed
{
    void install(PS2Runtime &runtime, const Addresses &addresses);
}
