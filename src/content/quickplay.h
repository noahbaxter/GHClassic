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

#include "addresses.h"

class PS2Runtime;

namespace gh2::quickplay
{
    void install(PS2Runtime &runtime, const Addresses &addresses);
}
