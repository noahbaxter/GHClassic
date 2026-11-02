#pragma once

// The menus' camera turn between screens: ui/metacam.milo's
// meta_cam_right_in as a screen enters and _out as it exits, each
// GHScreen's Enter (0x145cf8) and Exit (0x145f80) play where its
// CheckAnimateTransition (0x145dc8) found its animate_transition on.
//
// Which way is one flag for every screen (0x3da34c): the game turns it back
// for its own back press or a script's {backwards_anim}, and on again only
// once a screen that turns has come in, so going back into one that does
// not (quickplay's hub) left the next change turning back, and a script
// that forgot backwards_anim turned on into the screen it came from. Here
// each change sets it: back into a screen still on the way that led here,
// on into any other.
//
// Which changes turn is the game's own but for config/menus.toml's
// [transitions]: a screen named still never turns, nor two in one room
// between each other.
//
// No screen holds its input meanwhile beyond its turn: a component's
// animation (the store's grate, 1.2 s) plays on without holding it, and a
// press left to the UI selects at the next poll, not a third of a second on.

#include "addresses.h"

class PS2Runtime;

namespace gh2::transitions
{
    void install(PS2Runtime &runtime, const Addresses &addresses);
}
