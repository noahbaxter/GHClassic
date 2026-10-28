#pragma once

// The host thread's side of the Controllers screen (devices.h).

#include "host/seats.h"

#include <vector>

namespace gh2::input
{
    bool controllersScreen();
    // Each poll while it is up, `opened` on the first: what it shows, the
    // keys pressed on it, rebinds and edits.
    void screenPoll(const std::vector<Read> &reads, const bool *keys, bool opened, const Seats &seats);
}
