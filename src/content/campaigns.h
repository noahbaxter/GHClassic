#pragma once

// A campaign is one game played as itself inside the running game: its files
// answer first, its campaign, store and characters are the config's, its
// screens run its own scripts, and its career saves under its own name. gh2,
// the game disc alone, is always one; a game adds its own as a layer to
// stand in front (disc/ark.h): its disc, where that is GH2's code over its
// own files (gh80s/install.cpp), or files made in GH2's form over the game
// disc's (gh2x/install.cpp).
//
// A switch happens between screens, with no panel up (campaigns.dta's
// ghc_switch_screen), and is retail's own parts run again. What a campaign's
// files give is read the first time it is switched to and kept, and each
// switch takes a copy:
//
//   files     the campaign's layer searched first (disc/ark.h)
//   config    the root config's arrays that differ by game laid over the
//             live tree's in place: the game keeps pointers into it
//             (VideoProvider's constructor, 0x11b0c8, holds store's video),
//             so an array there stays the object it was
//   scripts   each {new Class name ...} in ui/ui.dta's (init ...) handed to
//             the object of that name by its virtual SetTypeDef, as DataNew
//             (0x2b4c40) hands it to a new one
//   strings   the Locale's table, as Locale::Init (0x2cb798) builds it
//             (content/locale.h)
//   campaign  ~Campaign (0x12d0c0) and Campaign() (0x12cf00) where it stands,
//             as MetaPanel's constructor (0x134178) makes it, then that
//             game's career out of the save (save/save.h)
//
//   {campaigns active}         the active campaign's game
//   {campaigns count}          how many there are
//   {campaigns has <game>}     TRUE if that game has a campaign
//   {campaigns switch <game> [<screen>]}  to that game's at the next UI poll,
//                              once no script is running, then to that screen;
//                              TRUE unless it is the one active or there is none
//   {campaigns second_character}  the second player's default outfit there
//   {campaigns owns <game> <item>}  TRUE if that game's item is there to use
//                              outside its own career: one its store does not
//                              sell, or one some band has bought in its career
//   {campaigns progress <game> <band>}  how far that band (from 0) is through
//                              that game's career, 0 to 100: the career songs
//                              passed on the difficulty it has passed most on

#include "addresses.h"

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

class PS2Runtime;

namespace gh2::campaigns
{
    // `game` can be played as itself, with that layer in front.
    void add(const std::string &game, size_t front);

    const std::string &active();
    // Every campaign's game, gh2 first.
    std::vector<std::string> games();
    // The active game's venues, in its config's order.
    const std::vector<std::string> &venues();

    void install(PS2Runtime &runtime, const Addresses &addresses);
}
