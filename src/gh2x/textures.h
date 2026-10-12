#pragma once

namespace gh2
{
    // 360 GH2's textures on the game disc's track, HUD and menu scenes, when
    // its disc is mounted and the textures setting asks for them. After
    // ark::install, which indexes the discs, and after anything that reads
    // one of those scenes at startup for the PS2's own pixels.
    void installGh2xTextures();
}
