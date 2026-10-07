#pragma once

// The line under a difficulty on the career's difficulty screen.
//
// Campaign::GetStatusProgress(Difficulty) (0x12f828) fills the campaign's
// status_progress with the songs beaten and the songs there are: GH2's
// "(%i OF %i SONGS)". GH1's names the status first, "%s STATUS (%i OF %i
// SONGS)", which is filled here.

#include "addresses.h"

class PS2Runtime;

namespace gh2::status_line
{
    void install(PS2Runtime &runtime, const Addresses &addresses);
}
