#pragma once

// The campaign picker's poster and game cases (campaigns.dta), from the pack
// tools/campaign_art.py builds into the binary: the GH2 Demo's amp poster
// in place of the speed picker's, and for each game the pack has a wrap of,
// a DVD case in the picker's scene (ui/sel_speed.milo):
//
//   ghc_case_<game>_sleeve.mesh   the wrap, Mat ghc_case_<game>.mat
//   ghc_case_<game>_shell.mesh    the plastic, Mat ghc_shell_<game>.mat
//   ghc_cover_<game>.mesh         its front cover alone, flat, the case's
//                                 size, Mat ghc_case_<game>.mat
//   ghc_engine.mesh               a ring 1 across, Mat ghc_engine.mat, its
//                                 colour in its verts: the note engine's mark
//
// both under speed_poster.grp until a script moves them, a case's middle at
// its origin, x across its front, z up, its front toward -y, about 2.4 by
// 3.4. Each picture is drawn by the host at the pack's size
// (render/texture_store.h). The case is "Silent Hill 3 Ps2 Game cover"
// (https://skfb.ly/oR9ZT) by eyewideasca, CC-BY 4.0.
namespace gh2::campaign_case
{
    // After ark::install, which indexes the discs.
    void install();
}
