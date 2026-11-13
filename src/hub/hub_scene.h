#pragma once

// The quickplay pre-show's venue (quickplay_setup.dta): quickplay's character
// select (content/outfits.h pickerScene), where the show is set up, given
//
//   ghc_venue_<id>.mesh  each mounted game's venue's poster, by the venue's
//                        id ({campaigns venues}, content/sources.h), and
//                        ghc_venue_<game>_unknown.mesh song defaults' one
//                        (hub/venue_posters.h), flat, 2 across, copied
//                        from a portrait and drawn after them, in white
//                        rather than their ink, until a script moves it, Mat
//                        .mat and Tex .tex alike
//
// A poster's tape margin is clear in its alpha: its Mat wants blend 3.
namespace gh2::hub_scene
{
    // After ark::install, which indexes the discs.
    void install();
}
