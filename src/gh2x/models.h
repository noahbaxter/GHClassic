#pragma once

#include "milo/milo.h"

#include <functional>
#include <string>

namespace gh2
{
    // A change to the 360 scene served at a PS2 path, made once it is
    // converted, as ark::addFix makes one to the PS2's own. Before
    // installGh2xModels, which starts converting the pickers' scenes.
    void addGh2xSceneFix(const std::string &path, std::function<void(milo::Dir &)> fix);

    // 360 GH2's venues, characters and guitars in place of the game disc's,
    // when its disc is mounted and settings::hd says so of each: each
    // scene under world/ and char/ is converted when the game first asks for
    // it (gh2x/scene.h), at the PS2's path, those only the 360 has (its
    // characters' viseme and menu clips, which theirs name) included. After
    // ark::install, and after anything that reads those scenes at startup for
    // the PS2's own.
    void installGh2xModels();

    // Whether the 360 has its own of a game disc scene whose path starts
    // `prefix`: "char/punk1/", an outfit's.
    bool gh2xHasModel(const std::string &prefix);
}
