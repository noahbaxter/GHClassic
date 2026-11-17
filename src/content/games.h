#pragma once

// The games GH Classic draws content from: gh2 (the game disc, always
// there), gh1, gh80s and gh2x (360 GH2), each mounted when a disc of any of
// its supported releases is (config/executables.toml's [[release]] game and
// role). Anything a game adds, its songs, outfits, venues or career, goes in
// only when it is mounted, and anything that offers it asks here first.
//
// Any other PS2 disc of GH2's form is a hack, one more game: one of no
// release's serial, or a release's with another archive header (a slot
// swap). Its id is its own save folder's ("BAGHCE-68878" is ghce68878),
// which no other game's shares, else its header's ("h" and 8 hex digits).
//
//   {mounted <game>}  TRUE if that game's disc is mounted
//   {hacks [career]}  the hacks mounted, in the order added; with career,
//                     those with a career of their own

#include "addresses.h"

#include <cstddef>
#include <filesystem>
#include <optional>
#include <set>
#include <string>
#include <vector>

class PS2Runtime;

namespace gh2::games
{
    // Every other game's disc among `images`, the first of each game, added
    // to the archive in the order their archives are searched whatever the
    // images are called: Rocks the 80s, GH1, 360 GH2, then the hacks. An
    // image that is neither is named in the log. Before ark::install.
    void mount(const std::vector<std::filesystem::path> &images);
    // Each hack's id, from its files. After ark::install.
    void nameHacks();

    // Its disc's index in the archive (ark::readFile), once ark::install has
    // run, or none when not mounted.
    std::optional<size_t> disc(const std::string &game);

    bool mounted(const std::string &game);

    // The hacks mounted, once named.
    std::vector<std::string> hacks();
    // Whether that indexed disc is a hack's.
    bool isHack(size_t disc);
    // Whether that hack has a career of its own: its career screens
    // (ui/career.dta). One with none (marfGH) is a setlist alone.
    bool hasCareer(const std::string &hack);
    // What the menus call it: its image's file name, which whoever added it
    // chose.
    std::string name(const std::string &hack);
    // What that hack's executable defines where GH2's defines _SHIP.
    std::string build(const std::string &hack);
    // Every name a Deluxe executable defines there: GH2DX's, the 80s',
    // Mega, Custom Edition, Drums, the two debug builds and GH1 Redux. One
    // set of scripts serves them all, a build's setlist and screens chosen
    // by these.
    const std::set<std::string> &builds();

    void install(PS2Runtime &runtime, const Addresses &addresses);
}
