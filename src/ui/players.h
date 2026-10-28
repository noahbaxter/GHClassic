#pragma once

#include <string>

class PS2Runtime;

namespace gh2
{
    // Multiplayer's screen as where players join (dta/players.dta): who
    // holds each seat under the mode buttons, P2 joining with Start and
    // yellow moving a player up a seat, and no mode until two have.
    void installPlayers(PS2Runtime &runtime);

    // A seat as a player is shown it ahead of what holds it: "P1  ".
    std::string seatTag(int seat);
}
