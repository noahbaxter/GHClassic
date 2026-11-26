#pragma once

// No Fail, a modifier on Quick Play's MODIFIERS screen ({no_fail}), never a
// setting: only a quick play song obeys it, not a career, practice or
// multiplayer. With it the rock meter running out ends no song. GH2's
// Performer::UpdateCrowdMeter calls Performer::LoseGame once the meter is
// below its lose level (and the player can lose), which ends the song lost;
// that call is skipped, the song plays on, and the run is kept marked failed
// (save/best.h), as Clone Hero keeps a no-fail run. So it shows, the rock
// meter powers down where it is (ui/dta/no_fail.dta), the highway's edges
// stay red and the crowd boos, its loop fading under that; at the next camera
// cut the audience is gone, and once the boo has rung out the crowd is silent.
// A restart puts it all back.

#include "addresses.h"

class PS2Runtime;

namespace gh2
{
    void installNoFail(PS2Runtime &runtime, const Addresses &addresses);
    // This play of the song ran the meter out and played on.
    bool failedThisPlay();
}
