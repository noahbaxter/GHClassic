#pragma once

// A custom chart's open chords (custom/gh2_chart.h, Gh2Gem::openChord):
// frets struck with an open, played as YARG plays them, with exactly those
// frets held, none below them (YargFiveFretGuitarEngine, open chords). GH2
// takes one fret by the highest held alone, so a lower one held beside it
// passes (Swing, 0x24d518); an open chord's is matched as a chord's is, by
// every fret held, strummed (Swing), fretted late (TryToFinishSwing) or
// hammered on (TryToHopo), under either note engine. Drawn by
// highway/extended/guitar/open_gems, its frets over an open's bar.

#include "addresses.h"

class PS2Runtime;

namespace gh2::guitar
{
    void installOpenChordHits(PS2Runtime &runtime, const Addresses &addresses);
}
