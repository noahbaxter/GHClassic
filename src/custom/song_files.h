#pragma once

// A custom song as the game reads it: its entry in the game's config while
// its row is near the list's window, and its files, songs/<id>/<id>.mid
// and the rest, and its album art, made from its folder when the game first
// reads them.

#include "custom/library.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

class PS2Runtime;
struct R5900Context;

namespace gh2::custom
{
    // The window's songs into the config before the list reads them, and
    // the longest unlisted out where there are too many (setlists::Live's
    // ready).
    void readyEntries(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, const std::vector<std::string> &ids);

    // The song being started, kept in the config and named by
    // playingSong() until the next: its files for the part it plays
    // (custom/songs.h base).
    std::string startSong(const std::string &id, std::shared_ptr<const Song> song);
    std::shared_ptr<const Song> playingSong();

    // Its files served as the game asks for them (ark::addMaker).
    void installSongFiles();
}
