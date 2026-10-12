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
// The screens either side fade as any do, and the next one's scenes load in
// the frame of the switch.
//
// Only a career is all one game's. Outside one GH2's campaign is active, with
// every game's content beside it, and the menus are the theme's: the main
// menu's screens and those it opens on its own, their scenes and their music
// (menusFrom), over GH2's. Quickplay, multiplayer and practice are the same
// whatever the theme. Going into a career is a switch to its game, menus
// and all; going back to the main menu is one to GH2's with the theme's
// menus; a change of theme is the menus alone.
//
//   {campaigns active}         whose menus are up: the career's game in one,
//                              else the theme
//   {campaigns live}           the active campaign's game: GH2 outside a career
//   {campaigns count}          how many there are
//   {campaigns has <game>}     TRUE if that game has a campaign
//   {campaigns switch <game> [<screen> [career]]}  to that game's menus
//                              outside a career, or with career into its
//                              career, at the next UI poll once no script is
//                              running, then to that screen; TRUE unless there
//                              is nothing to change or no such game. Either
//                              way that game becomes the theme. A career left
//                              for main_screen goes back to its menus outside
//                              it on its own.
//   {campaigns first_character}   the first player's default outfit there
//   {campaigns second_character}  the second player's default outfit there
//   {campaigns owns <game> <item>}  TRUE if that game's item is there to use
//                              outside its own career: one its store does not
//                              sell, or one some band has bought in its career
//   {campaigns progress <game> <band>}  how far that band (from 0) is through
//                              that game's career, 0 to 100: the career songs
//                              passed on the difficulty it has passed most on
//   {campaigns venues <game>}  that game's venues, as its config lists them
//   {campaigns venue_open <game> <venue>}  TRUE if a song may play there
//                              outside its career: its first, or one a band
//                              has unlocked in that game's career
//   {campaigns venues_from <game>}  the songs from here on play in that
//                              game's venues: its world files and its venues'
//                              scripts, until another or the game played's
//                              own are asked for, or a switch. Outside a
//                              career the screens around a song, the HUD and
//                              the loading tips are in_song_ui's game's, the
//                              venue's by default, and the gems gems' game's,
//                              the in-song look's by default (settings.h)
//   {campaigns venue_game}     whose venues those are
//
// Each campaign's own files, whichever is played, are under ghc/<game>/
// ("ghc/gh1/ui/sel_venue.milo"): GH2's are the game disc's.

#include "addresses.h"
#include "script.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

class PS2Runtime;

namespace gh2::campaigns
{
    // `game` can be played as itself, with that layer in front.
    void add(const std::string &game, size_t front);
    // The game disc's own campaign with that layer in front of it: files
    // made here over the disc's own.
    void over(size_t layer);

    // The active campaign's game: the career's in one, else gh2.
    const std::string &active();
    // Every campaign's game, gh2 first.
    std::vector<std::string> games();
    // That game's venues, the active game's by default, in its config's
    // order. None for a game with no campaign.
    const std::vector<std::string> &venues(const std::string &game = active());
    // The game whose venue a song plays in: the active one's unless
    // {campaigns venues_from} named another.
    const std::string &venueGame();

    // Whether `game`'s config has `item` (a character, outfit, guitar or
    // finish) and its career has it to use: one a band has bought there, or
    // one its store does not sell, unless it is earned another way (a
    // finish with its guitar, the Battle Axe by winning, the Grim Ripper's
    // guitar with him). False for a game with no campaign.
    bool unlockedIn(const std::string &game, const std::string &item);
    // Whether any game's career has that guitar or finish to use, a guitar
    // under either name where a game has its own for it. None if it is no
    // game's guitar or finish.
    std::optional<bool> guitarUnlocked(const std::string &item);
    // Whether `item` is a finish of another game's than the active one.
    bool lentFinish(const std::string &item);
    // A script that gives the live config's guitars every other game's
    // finishes of them, and whole each guitar it has none of, each once,
    // for the pickers outside a career.
    const std::string &lendFinishes();
    // A script for the finishes named as a guitar is (GH2's scythe, the
    // discs' controller), which the game cannot tell from the guitar: it is
    // unlocked whenever its guitar is. So the config has one only where it
    // is to be picked: outside a career where its own game's career has
    // its guitar (or the unlock-all cheat is on), in a career the active
    // game's. That makes each game's scythe and controller its own finish
    // of one guitar outside a career. After lendFinishes.
    std::string ownFinishes(uint8_t *rdram);
    // `game`'s own outfits, in its config's order.
    std::vector<std::string> outfits(const std::string &game);
    // In a career, where every pick is its own game's: the game's mode is
    // career, however it got there.
    bool inCareer(uint8_t *rdram);

    void install(PS2Runtime &runtime, const Addresses &addresses);
}
