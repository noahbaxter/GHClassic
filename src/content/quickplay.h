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
//                                    song and highway outside a career, kept in
//                                    settings.ini (content/campaigns.h)
//   {quickplay menu_music [<theme|gh1|gh2|gh80s|all>]}  whose menu loops
//                                    outside a career, from the next change
//                                    of menus (content/campaigns.h)
//   {quickplay menu_music_order [<playlist|loop|shuffle>]}  and how they
//                                    follow one another (ui/meta_music.cpp)
//   {quickplay characters [<sd|hd>]}  outside a career, the PS2's guitarists
//                                    and band or the 360's, kept in
//                                    settings.ini (settings::hd)
//   {quickplay guitars [<sd|hd>]}    and its guitars
//   {quickplay venues [<sd|hd>]}     and GH2's venues
//   {quickplay has_hd outfit <outfit>}  TRUE if the 360 has its own of that
//                                    outfit, by the game disc's name for it
//   {quickplay has_hd guitar <file>}  or of that finish, by its (outfit ...)
//   {quickplay model <outfit|guitar> <name> <sd|hd|default>}  that outfit's or
//                                    finish's look by itself, whatever the
//                                    setting says, for one player of two
//                                    (ui/dta/multiplayer.dta); 1 if it is the
//                                    360's now, and 2 more if that changed it
//                                    (what is on show is loaded again)
//   {quickplay model clear}          every one of those let go
//
// Outside a career (content/campaigns.h) an item is unlocked where any
// game's career has it. The guitars are GH2's with every game's finishes of
// them, the Grim Ripper's among them for anyone once he is a band's, and he
// plays any.

#include "addresses.h"

class PS2Runtime;

namespace gh2::quickplay
{
    void install(PS2Runtime &runtime, const Addresses &addresses);
}
