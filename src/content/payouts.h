#pragma once

// A career's cash, made to buy its store. The discs' stores cost what their
// careers pay to the dollar, so anything added to one could not be bought,
// and GH1's, priced for a wallet to each difficulty, is paid three times
// over by GH2's one wallet to a band.
//
// A game's cash table (config/campaign.dta's (cash ...)) is scaled so that
// every career song at five stars and every venue passed, on all four
// difficulties, pays what everything in its store (config/store.dta) costs:
// a quarter to each difficulty, Easy too, which the discs pay nothing. The
// game pays a song and a venue each difficulty's row at or under the one
// played (ProfileState::FinishSong 0x13ba20), so Expert alone earns it all.
//
// Medium's, Hard's and Expert's rows keep the disc's shape, each award in
// its proportion. Easy's is its stars alone, since its career stops a venue
// short, with what rounding leaves on its first venue. The cash a new
// career starts with (none on Easy) is counted in.

#include "formats/dtb.h"

namespace gh2
{
    // `campaign`'s cash scaled to `store`'s prices, both one game's config
    // as it will be served: called once the store has everything it sells.
    // False, with nothing changed, if either is not that.
    bool scalePayouts(dtb::Node &campaign, const dtb::Node &store);

    inline constexpr const char kCareerScript[] = "ui/gen/career.dtb";

    // The career hub's script (ui/career.dta) with Easy's cash shown and
    // its store open, which the discs hide and shut: Easy earned nothing to
    // spend. False if it is not that script.
    bool openEasyStore(dtb::Node &script);
}
