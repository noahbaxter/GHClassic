#pragma once

// Quickplay across games (ui/dta/quickplay_pickers.dta): a song played as
// its own game's, or with the guitarist, guitar and venue picked.
//
//   {quickplay song_defaults}        TRUE while a song plays as its own game's
//                                    guitarist and venue, none picked
//   {quickplay song_defaults <0|1>}  that, kept in settings.ini
//   {quickplay outfit <game>}        the first player's outfit that game boots
//                                    with (its ui/main.dta), as it is named
//                                    here while another game is played
//   {quickplay pick <what> [<value>]}  quickplay's last pick of that (outfit,
//                                    guitar, skin, venue, venue_game), kept
//                                    for the session through switches of menus
//   {quickplay picked <what>}        TRUE once one has been picked
//   {quickplay quickplay <0|1>}      into quickplay or out of it. In it the
//                                    characters and guitars are GH2's with
//                                    every game's outfits among them, whatever
//                                    game's menus these are, and an item is
//                                    unlocked where any game's career has it.
//                                    A career keeps its own game's.

#include "addresses.h"

class PS2Runtime;

namespace gh2::quickplay
{
    void install(PS2Runtime &runtime, const Addresses &addresses);
}
