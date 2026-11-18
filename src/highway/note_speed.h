#pragma once

#include "addresses.h"

class PS2Runtime;

namespace gh2
{
    void installNoteSpeed(PS2Runtime &runtime, const Addresses &addresses);

    // Seat `seat`'s note speed, in tenths, and highway length: its
    // profile's (save/profiles.h), else the game's settings, hard and
    // expert's speed. Each player's track is built with its own.
    int seatSpeed(int seat);
    int seatLength(int seat);
}
