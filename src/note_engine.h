#pragma once

#include "addresses.h"

#include <string>
#include <vector>

class PS2Runtime;

namespace gh2
{
    // Every note engine, GH2's first.
    const std::vector<std::string> &noteEngines();
    // The one judging now: a career's own in one, else quickplay's
    // (save::quickplayEngine), until set.
    const std::string &noteEngine();
    // Judging from the next song loaded. The save's careers follow it only
    // through save::switchEngine, which calls this.
    void setNoteEngine(const std::string &engine);

    void installNoteEngine(PS2Runtime &runtime, const Addresses &addresses);
}
