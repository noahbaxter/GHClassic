#pragma once

// GH1's sounds where GH2's differ.

#include <cstddef>

namespace gh2::gh1
{
    // GH2's in-game bank with GH1's own star power and win stingers, and
    // silent where GH2 has a sound GH1 does not, onto `layer`.
    void addSounds(size_t layer, size_t disc);
}
