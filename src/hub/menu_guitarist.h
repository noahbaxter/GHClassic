#pragma once

#include "addresses.h"
#include "milo/milo.h"
#include "ps2_runtime.h"

// The quickplay pre-show's guitarist is the outfit's song figure
// (char/<c>/og/<c>.milo), as a song shows it: its guitar loader, hand IK,
// hand drivers and clips (ui/dta/preshow_guitarist.dta). A preview panel of
// its own loads it as ghcsong_<outfit>, which this serves the song figure's
// path for and lets load. It plays the song clips picked for it, again as
// preshow_pick_1.. in every song set (<c>_main.milo), each handing over to
// one of its song idles again as preshow_idle.
namespace gh2::menu_guitarist
{
    // Song set `main`'s picks (preshow_picks.dta, as `game`_<set>: sd or
    // gh1; else its default) in turn as preshow_pick_1.., the walk-on
    // walking in to its spot, the rest back on it when they hand over; and
    // its stillest idle with both hands on the guitar as preshow_idle. All
    // in real time, without events or tempo, so loaded whatever the figure
    // loads at; each goes on to the idle. False if it has no idle.
    bool addWalkOns(milo::Dir &main, const std::string &game);

    // After ark::install, which indexes the discs, and before
    // installGh2xModels, which takes its fixes to the 360's song sets.
    void install(PS2Runtime &runtime, const Addresses &addresses);
}
