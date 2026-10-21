#pragma once

// GH1's rules, in parts installNoteEngine installs. Each is GH2's own code
// with GH1's differences put back, listed atop its file.

#include "addresses.h"
#include "gameplay/note_engine/note_engine.h"

#include <cstdint>
#include <type_traits>

class PS2Runtime;
struct R5900Context;

namespace gh2::gh1_rules
{
    // The guitar watcher's: HOPOs, sustains, strums, misses and passes.
    void installWatcher(PS2Runtime &runtime, const Addresses &addresses);
    // The player's: star phrases and the multiplier a hit scores at.
    void installStarPower(PS2Runtime &runtime, const Addresses &addresses);
    // scoring.dta's numbers, and GH2's put back under GH2's engine.
    void installScoring(PS2Runtime &runtime, const Addresses &addresses);

    // A hook that only GH1's rules need, as installed: under GH2's it does
    // nothing and an Override's call goes to the original.
    template <auto Hook>
    auto gh1Only(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if constexpr (std::is_same_v<decltype(Hook(rdram, ctx, runtime)), bool>) // an Override's
            return noteEngine() == NoteEngine::kGh1 && Hook(rdram, ctx, runtime);
        else if (noteEngine() == NoteEngine::kGh1)
            Hook(rdram, ctx, runtime);
    }
}
