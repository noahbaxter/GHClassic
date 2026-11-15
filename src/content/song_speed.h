#pragma once

// Songs played faster or slower, in quickplay, multiplayer and practice
// alike. The game plays at {song_speed} percent (BeatMatch::SetPracticeSpeed,
// after every BeatMatch::Restart), and a song block's audio name passed
// through {song_speed name <name>} is its audio at that speed: made as GH2
// made its practice files, stretched to the speed's length with its pitch
// kept, then written at its rate over the speed, as the game plays a slowed
// song's stream that much slower. "ghc_speed/<percent>/<a song's .vgs>" is
// that .vgs at that percent, 10 to 500, made as the game reads it. With
// maintain_pitch off the song's own file plays at the speed, like a tape.
//
// Practice's audio is "ghc_speed/50/...", made at 50% once and played at
// any speed, changed as it plays, its pitch put back in the mix as it is
// heard (synth::keepPitch), so nothing it has read ahead is wrong. It also
// plays the part alone where it has channels of its own ({song_speed solo},
// ui/dta/game_settings.dta's ghc_practice_solo).

#include "addresses.h"

class PS2Runtime;

namespace gh2::song_speed
{
    void install(PS2Runtime &runtime, const Addresses &addresses);
    // The speed songs play at, in percent.
    int percent();
}
