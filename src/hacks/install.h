#pragma once

namespace gh2
{
    // Each hack mounted named, its scripts and text as its own executable
    // reads them, and GH2's UI types gaining its own. After ark::install,
    // before anything reads GH2's UI objects (gh1/menus.cpp copies them).
    void readHacks();

    // Each hack mounted (content/games.h) as a campaign and a quickplay
    // setlist of its own, and a theme after the official games'
    // (main_menu.dta). After ark::install, which indexes the discs.
    void installHacks();
}
