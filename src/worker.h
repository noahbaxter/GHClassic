#pragma once

// One thread beside the game's, for work the game thread would otherwise
// stop a frame for: a scene converted or a picture decoded before it is
// asked for.

#include <functional>

namespace gh2::worker
{
    // Run after every job given before it.
    void run(std::function<void()> job);
}
