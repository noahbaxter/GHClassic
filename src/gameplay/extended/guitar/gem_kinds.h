#pragma once

// A custom song's gems as its chart has them, put into GH2's as they load: each
// strummed or hammered on as the engine judging decides it (GH2's or GH1's
// rule, GameGemList::WillBeNoStrum) under the song's own threshold where it has
// one, unless the chart forces it, flips it, or it is a tap or an open; each
// open a gem with no fret; and a sustain as long as its chart holds it, past
// the next gem (held by gameplay/extended/guitar/sustain_hold). How they are
// played is gameplay/extended/guitar/hammer_ons'. Only for a song whose
// midi_file is one of ours; every other song loads as ever.

#include "addresses.h"
#include "custom/gh2_chart.h"

#include <array>
#include <map>
#include <optional>
#include <string>

class PS2Runtime;
struct R5900Context;

namespace gh2::guitar
{
    using custom::Gh2Gem;

    // `midi`, as a song's midi_file names it, has these gems, and natural
    // HOPOs under that threshold (Gh2Chart's) where it has one.
    void setGems(const std::string &midi, const std::array<std::map<uint32_t, Gh2Gem>, 4> &gems,
                 std::optional<uint32_t> hopoThreshold);

    // Whether the song loaded last is a custom song (a Clone Hero chart, read
    // through custom/gh2_chart), in any mode; never a song off a disc.
    bool playingCustom();

    // The lane an open is hammered on as, and drawn on.
    constexpr uint32_t kOpenLane = 2u;

    // The song playing's gem on that track (SongData's), at that tick (GH2's,
    // 480 a beat) on those frets as loaded, as its chart has it; none for a
    // song not ours. Before the track's difficulty is known, the first
    // difficulty with such a gem's. And whether it is a tap.
    const Gh2Gem *chartGem(uint32_t track, uint32_t tick, uint8_t frets);
    bool isTap(uint32_t track, uint32_t tick, uint8_t frets);

    // GuitarTrackWatcherImpl's gem i (its GameGem), that gem as its chart has
    // it (chartGem), the watcher's now in ms, by its clock, and its gem
    // closest to now not yet played, -1 for none.
    uint32_t watcherGem(uint8_t *rdram, uint32_t watcher, int32_t index);
    const Gh2Gem *watcherChartGem(uint8_t *rdram, uint32_t watcher, int32_t index);
    float watcherNow(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t watcher);
    int32_t closestUnplayed(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t watcher);

    void installGemKinds(PS2Runtime &runtime, const Addresses &addresses);
}
