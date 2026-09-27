// GH2's hooks, installed once the runtime has loaded a matching executable.

#include "addresses.h"
#include "game_overrides.h"
#include "null_movie.h"
#include "null_rnd.h"
#include "null_synth.h"
#include "render/native_rnd.h"

namespace
{
    void applySlus21447(PS2Runtime &runtime)
    {
        gh2::installNullRnd(runtime, gh2::kSlus21447);
        gh2::installNativeRnd(runtime, gh2::kSlus21447);
        gh2::installNullSynth(runtime);
        gh2::installNullMovie(runtime, gh2::kSlus21447);
    }
}

// Matched on the entry point, which the symbolized ELF keeps from retail.
PS2_REGISTER_GAME_OVERRIDE("Guitar Hero II (USA)", "", gh2::kSlus21447.entry, 0u, applySlus21447)
