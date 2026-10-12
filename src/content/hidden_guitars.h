#pragma once

// Guitars and finishes the game disc holds and its config never offers, in
// its own career's store (config/guitars.dta and store.dta, as a layer over
// the disc's), and in the 360's and the 80s', whose discs hold them too:
//
//   exp_peppermint, exp_lipstick  two finishes in the Explorer's scene
//                       (xplorer.milo) that no skin names
//   lp_ebony, lp_gold, fb_copper, fb_hippie, mod_black, mod_dragon,
//   cor_white, cor_snake, es_ebony  nine of GH1's finishes left in the
//                       scenes of the bodies GH2 kept, which no skin names
//                       either. GH1's career sells them from these scenes
//                       too (gh1/guitars.h), under the same keys
//   guitar_red_octane   the controller as a guitar: a cheat's in GH1, and in
//                       the config here as a cheat's that no cheat picks.
//                       Earned as the reward guitars are, by passing hard's
//                       tour or expert's, then bought. Each game's is the
//                       one it came with and no other: GH1's the black one
//                       on the discs, the others' GH2's red, painted from
//                       it. Outside a career they are one guitar's finishes
//                       (content/campaigns.h)
//   air_guitar          what the air guitar cheat swaps in (long_cheats.dta):
//                       the hands' bones and no guitar
//
// Their names and blurbs, with those of GH1's guitar that no disc names
// (gh1/guitars.h), are guitar_copy.dta's: a token left "" there is not
// added, and shows in game as its own name.

#include "formats/dtb.h"
#include "gh1/rig.h"
#include "milo/milo.h"

namespace gh2
{
    // A GH2-form guitar scene whose guitar.mesh is turned about the neck
    // from how the disc's guitars stand under the guitar spot (`stand`, one
    // of theirs' local), so that on show it faces away: its verts take the
    // turn. False if its mesh cannot be read.
    bool faceGuitarFront(milo::Dir &scene, const gh1::Xfm &stand);

    // Those in a game's guitars and its store's, for every game whose disc
    // holds them: all four in GH2's, the 360's and the 80s', and with `gh1`
    // the controller and the air guitar in GH1's. False, with nothing
    // changed, if the two are not that game's config.
    bool offerHiddenGuitars(dtb::Node &guitars, dtb::Node &store, bool gh1);

    // guitar_copy.dta's text. Before any game adds its own for the same
    // tokens (GH1 names the finishes it shares by the whole guitar): the
    // first added is the one read.
    void installGuitarCopy();

    // After ark::install, and before locale::install.
    void installHiddenGuitars();
}
