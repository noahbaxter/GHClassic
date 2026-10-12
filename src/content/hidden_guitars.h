#pragma once

// Guitars and finishes the game disc holds and its config never offers, in
// its own career's store (config/guitars.dta and store.dta, as a layer over
// the disc's):
//
//   exp_peppermint, exp_lipstick  two finishes in the Explorer's scene
//                       (xplorer.milo) that no skin names
//   guitar_red_octane   the controller as a guitar: a cheat's in GH1, and in
//                       the config here as a cheat's that no cheat picks.
//                       Earned as the reward guitars are, by passing hard's
//                       tour or expert's, then bought
//   air_guitar          what the air guitar cheat swaps in (long_cheats.dta):
//                       the hands' bones and no guitar
//
// Their names and blurbs, with those of GH1's guitar that no disc names
// (gh1/guitars.h), are guitar_copy.dta's: a token left "" there is not
// added, and shows in game as its own name.

#include "gh1/rig.h"
#include "milo/milo.h"

namespace gh2
{
    // A GH2-form guitar scene whose guitar.mesh is turned about the neck
    // from how the disc's guitars stand under the guitar spot (`stand`, one
    // of theirs' local), so that on show it faces away: its verts take the
    // turn. False if its mesh cannot be read.
    bool faceGuitarFront(milo::Dir &scene, const gh1::Xfm &stand);

    // After ark::install, and before locale::install.
    void installHiddenGuitars();
}
