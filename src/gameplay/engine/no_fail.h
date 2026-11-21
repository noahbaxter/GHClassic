#pragma once

// no_fail on: in quick play the rock meter running out ends no song. GH2's
// Performer::UpdateCrowdMeter calls Performer::LoseGame once the meter is
// below its lose level (and the player can lose), which ends the song lost;
// that call is skipped, the song plays on, and the run is kept marked failed
// (save/best.h), as Clone Hero keeps a no-fail run.

#include "addresses.h"

class PS2Runtime;

namespace gh2
{
    void installNoFail(PS2Runtime &runtime, const Addresses &addresses);
    // This play of the song ran the meter out and played on.
    bool failedThisPlay();
}
