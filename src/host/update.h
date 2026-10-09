#pragma once

namespace gh2::update
{
    // Asks GitHub whether a newer release of this build's track is out, and
    // if one is asks the player, in the system's own dialog, whether to
    // download it. True when they took it, its page then open in the browser:
    // the game should not start. A build that is no release (its version is
    // no tag) does not ask.
    bool offer();
}
