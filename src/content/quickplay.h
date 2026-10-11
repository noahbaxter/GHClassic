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
//   {quickplay look <character> [<outfit>]}  the outfit last looked at on
//                                    that character in quickplay's picker
//   {quickplay in_song_ui [<venue|gh1|gh2|gh80s>]}  whose screens around a
//                                    song and HUD outside a career, kept in
//                                    settings.ini (content/campaigns.h)
//   {quickplay gems [<auto|gh1|gh2>]}  and whose gems
//
// Outside a career (content/campaigns.h) an item is unlocked where any
// game's career has it.

#include "addresses.h"

class PS2Runtime;

namespace gh2::quickplay
{
    void install(PS2Runtime &runtime, const Addresses &addresses);
}
