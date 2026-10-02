#pragma once

// GH1's animation clips as GH2's.

#include "formats/dtb.h"
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

        // The sets a song plays (char/<gh2Set>/anims/<gh2Set>_main, _fret and
        // _strum), as GH2's base has them, each clip's motion GH1's: its own
        // where GH1 has one by name, GH2's body under GH1's arms for GH2's
        // band moments, and otherwise a GH1 clip of the same situation and
        // tempo. `macros` holds GH1's anim sets (charsys.dta's).
        struct SongSets
        {
            milo::Bytes main, fret, strum;
        };
        std::optional<SongSets> songSets(const Guitarist &guitarist, const std::string &gh2Set, const milo::Dir &gh1,
                                         const dtb::Macros &macros);
    }
}
