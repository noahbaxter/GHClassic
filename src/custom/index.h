#pragma once

// What a scan leaves in our own songs/ folder (custom/library.h): the index
// the next start reads instead of walking the chart folders again, and for
// the player, badsongs.txt and songs.csv.

#include "custom/library.h"

#include <filesystem>
#include <map>
#include <vector>

namespace gh2::custom
{
    // The songs the last scan found, playable or not.
    std::vector<Song> readIndex(const std::filesystem::path &file);
    void writeIndex(const std::filesystem::path &file, const std::vector<Song> &songs);

    // badsongs.txt: each song that does not play, by its Reject, with what
    // is wrong where the heading does not say it all.
    void writeBadSongs(const std::filesystem::path &file, const std::vector<Song> &songs);

    // songs.csv: each song that plays, a row each, as a spreadsheet opens
    // it: its name, artist, album, year, genre, charter, length, the parts
    // and difficulties it has, and its folder. Not those in `copied`
    // (library.h's copies).
    void writeSongList(const std::filesystem::path &file, const std::vector<Song> &songs,
                       const std::map<std::filesystem::path, std::filesystem::path> &copied);
}
