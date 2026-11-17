#pragma once

// What a menu screen change loads, kept and quick.
//
// A screen loads its panels as it comes (UIScreen::LoadPanels, 0x2134a0) and
// the one it left lets its own go (UnloadPanels, 0x213508), each panel freed
// once no screen holds it (UIPanel::CheckUnload, 0x20e0d0): every change into
// a picker loaded it again, its models read as the screen faded out. Here a
// menu panel no screen holds stays loaded while the menus are up (the meta
// panel, which every menu screen has and no song's does), and all are let go
// with meta, as a song loads.
//
// And the game's own work in a change (GHScreen's Enter and Exit) and in
// loading (LoadMgr::Poll) runs unpaced: its code runs some three times faster
// than its cycles count for, and the scheduler slept the difference away
// after, the frame late.

#include "addresses.h"

class PS2Runtime;

namespace gh2::screen_loads
{
    void install(PS2Runtime &runtime, const Addresses &addresses);
}
