#pragma once

// A custom chord whose lanes end apart (custom/gh2_chart.h's laneMs) drawn
// so: each lane's tail as long as its own sustain.

#include "addresses.h"

class PS2Runtime;

namespace gh2::guitar
{
    void installSustainTails(PS2Runtime &runtime, const Addresses &addresses);
}
