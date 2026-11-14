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

    // TRUE once what the song's preview plays of its stems is read into
    // memory, for it to play with no wait on its disk. Until then it is
    // read on a thread of its own, the last song asked about the one read,
    // and the rest of its stems after, for playing it. The song started
    // (song_files.h) opens with its stems read whole.
    bool stemsReady(const Song &song);
    // These songs' album art read on that thread, in this order, and held
    // while they are the ones last wanted: the first before any preview.
    void wantArt(const std::vector<std::shared_ptr<const Song>> &songs);
    // A song's art's bytes once read there, none before: a song folder on
    // a network drive a scan is busy with can take a second to give them.
    std::shared_ptr<const std::vector<uint8_t>> heldArt(const Song &song);
    // Back once that thread has read the preview and art last asked for,
    // for a scan to leave the disk to it: a network drive serves the
    // scan's many requests in flight before any of the list's.
    void waitForList();
}
