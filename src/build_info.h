#pragma once

// What this build is, set when it is configured (tools/ghc.py).

#include <string>

namespace gh2::build
{
    // The release it was built as ("v0.6", "v0.6-exp.2"), or what git
    // describes an untagged tree as.
    extern const char *const kVersion;

    // The experimental track, every game in one: installed beside the stable
    // one, with saves and settings of its own.
    extern const bool kExperimental;

    // The version as the main menu shows it: a release's tag alone; a build
    // past it, or with changes, as when it was configured and the tag, the
    // tag last so it ends where a release's does ("2026-10-08 17:20  v0.5"),
    // dev in its place when there is no tag.
    std::string shownVersion();
}
