#pragma once

#include "addresses.h"

class PS2Runtime;

namespace gh2
{
    // 360 GH2's songs as quickplay's gh2x setlist, when its disc is mounted:
    // its own charts and audio, in its own tiers. After ark::install, which
    // indexes the discs.
    void installGh2x();

    // Its textures, models and look (gh2x/textures.h, models.h, focus.h,
    // shadows.h), each when its setting asks for it. After anything that
    // reads the game disc's scenes at startup for the PS2's own, and after
    // the native renderer's hooks.
    void installGh2xAssets(PS2Runtime &runtime, const Addresses &addresses);
}
