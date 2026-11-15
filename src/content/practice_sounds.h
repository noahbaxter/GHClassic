#pragma once

// Practice's metronome and note click, Noah's own recordings
// (tools/procedural/practice_sounds.py), played by the mix at the song's
// time to the sample (synth/timeline.h): a click as the music reaches each
// of player 1's notes (a chord or open one note), and one each beat of the
// song's MIDI, its bars' first another.
//
//   {practice_clicks start <song>}  that song's practice begun (its beats
//   read from its MIDI); {practice_clicks stop} over; {practice_clicks
//   played} how many the mix has started.
//   {practice_option metronome|note_click|solo_stems [<0 or 1>]}
//   the setting, set first if given: a click each beat, one each note, and
//   the part played alone on its own stem with drum clicks where it can be.

#include "addresses.h"

class PS2Runtime;

namespace gh2::practice_sounds
{
    void install(PS2Runtime &runtime, const Addresses &addresses);
}
