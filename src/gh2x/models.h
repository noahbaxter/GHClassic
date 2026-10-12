#pragma once

namespace gh2
{
    // 360 GH2's venues, characters and guitars in place of the game disc's,
    // when its disc is mounted and the models setting asks for them: each
    // scene under world/ and char/ is converted when the game first asks for
    // it (gh2x/scene.h), at the PS2's path, those only the 360 has (its
    // characters' viseme and menu clips, which theirs name) included. After
    // ark::install, and after anything that reads those scenes at startup for
    // the PS2's own.
    void installGh2xModels();
}
