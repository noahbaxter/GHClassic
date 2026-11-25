#pragma once

// Whether a venue has a crowd: everything about its leaving and coming back,
// its look and its sound, is here and behind setCrowd.
//
// setCrowd(false): the crowd boos and walks out. The boo plays out whole, the
// loop under it fades, nothing else of theirs is heard (claps, groans, cheers,
// a change of mood) and at the next camera cut all of them are gone, flat
// crowds built into the venue too (ui/dta/crowd_presence.dta lists those).
// setCrowd(true): all of it back at once.
//
// {crowd_presence drawn}: how many times the crowd drew since last asked.
// {crowd_presence gone}: 1 once they have walked out, till they're back.

#include "addresses.h"

#include <cstdint>

class PS2Runtime;
struct R5900Context;

namespace gh2::crowd_presence
{
    void install(PS2Runtime &runtime, const Addresses &addresses);
    // Whether that changed anything.
    bool setCrowd(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, bool enabled);
    // The song left with the crowd gone: their sound put back and off for
    // the menus, while they stay out of sight as its venue fades out;
    // setCrowd(true) at the next song, a restart's too, brings them back.
    // False if they never left.
    bool leaveVenue(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
}
