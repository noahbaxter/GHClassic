#pragma once

class PS2Runtime;

namespace gh2
{
    // The Controllers screen (dta/controllers.dta): every connected device,
    // the one last pressed shown on the tutorial's guitar lighting up as it
    // is played, and a row per action to rebind from the keyboard.
    void installControllers(PS2Runtime &runtime);
}
