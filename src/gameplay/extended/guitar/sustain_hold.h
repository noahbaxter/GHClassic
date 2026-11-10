#pragma once

// A custom song's sustains as Clone Hero holds them: any number at once, each
// started and ended on its own (custom/gh2_chart.h writes each lane as long
// as its chart has it, to the next gem on that lane). The frets held for them
// are left out when the next gems are matched, so a gem is played over,
// under or beside them; letting a fret go lets go of the sustains on it.
//
// GH2 holds one sustain a player, its watcher's (TrackWatcherImpl +0x20) scored
// by its Player's HeldNote (+0xcc), and lets it go at the next hit. Here GH2
// holds none: each sustain hit is taken into the watcher's own list as it is
// hit, scored by GH2's CheckHeldNote in the Player's place, let go through
// SendReleaseGem as GH2 lets one go (the score, the track's sound, its tail),
// and the last hit whammied as GH2 whammies its own. A chord whose lanes end
// apart is held as long as its longest, each lane let go freely once its own is
// done, scored by each lane's length (its tails,
// highway/extended/guitar/sustain_tails.h). Each fret's burn flame keeps
// burning while a sustain is held on it
// (highway/extended/guitar/sustain_flames.h). Only for a song of ours.

#include "addresses.h"

#include <cstdint>

class PS2Runtime;

namespace gh2::guitar
{
    // The frets the watcher holds for sustains, those of `next` (the gem it
    // is to play, which ends any on them) aside.
    uint32_t sustainedLanes(uint8_t *rdram, uint32_t watcher, uint8_t next);
    // Those it holds at `now`, a split chord's lanes done by then aside.
    uint8_t heldLanes(uint8_t *rdram, uint32_t watcher, float now);
    // The watcher of that track, as it last hit a gem; 0 before.
    uint32_t trackWatcher(uint32_t track);

    // Every sustain forgotten, as a song loads.
    void forgetSustains();

    void installSustainHold(PS2Runtime &runtime, const Addresses &addresses);
}
