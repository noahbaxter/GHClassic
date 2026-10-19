#pragma once

#include "milo/milo.h"

#include <string>

namespace gh2
{
    // Each game's "Classic" under its logo, built into the scenes that draw
    // the logo, `screen` main or splash: a part of the scene, it loads,
    // moves and draws with the rest of it. False for another screen, or one
    // it cannot be put in.
    bool addClassicLogo(milo::Dir &scene, const std::string &game, const std::string &screen);

    // That for GH2's and the 80s' own main and splash scenes, as their discs
    // give them; GH1's are built (gh1/menus.cpp). After ark::install.
    void installClassicLogo();
}
