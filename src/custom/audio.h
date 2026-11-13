#pragma once

// A custom song's stems as one .vgs, a pair of channels each part: the
// backing (song, drums, vocals, keys, crowd, the other guitar or bass mixed)
// on 0 and 1, the played part on 2 and 3. A song of one stem has it all on
// 2 and 3; with more and none of its own the played part is silence, so a
// miss mutes nothing.

#include "custom/library.h"
#include "disc/ark.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace gh2::custom
{
    // `path` (songs/<name>/<name>.vgs) made from the song's stems, with
    // `played` the stem name PART GUITAR is ("guitar", or "bass" for a song
    // with only a bass part), as the game first reads it (ark::addMaker).
    std::optional<ark::Made> madeMix(const std::string &path, const Song &song, const std::string &played);

    // TRUE once every stem of the song is read into memory, for a mix of it
    // to open with no wait on its disk. Until then they are read on a
    // thread of their own, the last song asked about the one read, its
    // album art first.
    bool stemsReady(const Song &song);
    // That art's bytes once read there, none before: a song folder on a
    // network drive a scan is busy with can take a second to give them.
    std::shared_ptr<const std::vector<uint8_t>> heldArt(const Song &song);
}
