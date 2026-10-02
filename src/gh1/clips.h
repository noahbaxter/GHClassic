#pragma once

// GH1's animation clips as GH2's.

#include "gh1/guitarist.h"
#include "milo/milo.h"

#include <optional>
#include <string>

namespace gh2
{
    namespace gh1
    {
        // The clip set the picker plays, as GH2's base has it (its bones and
        // filter, its enter clip opening the door) with GH1's own picker
        // idle in every clip, leading into ui_loop where GH1's graph loops.
        std::optional<milo::Bytes> pickerSet(const Guitarist &guitarist, const std::string &gh2Set,
                                             const milo::Dir &gh1);
    }
}
