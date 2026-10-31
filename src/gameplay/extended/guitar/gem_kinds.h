#pragma once

// A custom song's gems as its chart has them, put into GH2's as they load:
// each strummed or hammered on as the engine judging decides it (GH2's or
// GH1's rule, GameGemList::WillBeNoStrum) under the song's own threshold
// where it has one, unless the chart forces or flips it. Only for a song
// whose midi_file is one of ours; every other song loads as ever.

#include "addresses.h"
#include "custom/gh2_chart.h"

#include <array>
#include <map>
#include <optional>
#include <string>

class PS2Runtime;

namespace gh2::guitar
{
    using custom::Gh2Gem;

    // `midi`, as a song's midi_file names it, has these gems, and natural
    // HOPOs under that threshold (Gh2Chart's) where it has one.
    void setGems(const std::string &midi, const std::array<std::map<uint32_t, Gh2Gem>, 4> &gems,
                 std::optional<uint32_t> hopoThreshold);

    void installGemKinds(PS2Runtime &runtime, const Addresses &addresses);
}
