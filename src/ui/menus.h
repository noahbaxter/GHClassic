#pragma once

class PS2Runtime;

namespace gh2
{
    // Changes to the game's menus, made in its own script over its panels.
    // Before locale::install.
    void installMenus(PS2Runtime &runtime);
}
