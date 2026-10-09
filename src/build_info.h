#pragma once

// What this build is, set when it is configured (tools/ghc.py).

namespace gh2::build
{
    // The release it was built as ("v0.6", "v0.6-exp.2"), or what git
    // describes an untagged tree as.
    extern const char *const kVersion;

    // The experimental track, every game in one: installed beside the stable
    // one, with saves and settings of its own.
    extern const bool kExperimental;
}
