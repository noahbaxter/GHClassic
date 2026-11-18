#pragma once

// A custom chart's trill and tremolo lanes (custom/chart.h) judged as YARG
// judges them (BaseEngine.Generic, GuitarEngine), under either note engine:
// a lane's note gone by unplayed is hit for the player while they played
// the lane in the last 160 ms, its first note excepted, which they hit
// themselves. Playing it is a strum or a fret pressed with the lane's
// frets: a tremolo's mask (a single note's fret as the highest held), a
// trill's two in turn, the hit on its first note starting it. A strum that
// hits nothing is no miss while it plays the lane, nor anywhere within 80 ms
// of a lane's first or last note.

#include "addresses.h"

class PS2Runtime;

namespace gh2::guitar
{
    void installLaneHits(PS2Runtime &runtime, const Addresses &addresses);
}
