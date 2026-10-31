#pragma once

// A setlist longer than the song list's paper, held a window at a time
// (content/setlists.h's scroll, jump and random ops): which rows the list
// has, which of them it shows, and the song it is on, kept there as the
// rows change.

#include "content/setlists.h"

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace gh2::setlist_window
{
    // The song list's paper (sel_song.view) is 70 frames, one a row and
    // one a header, the last its foot: past that the rows scroll on with no
    // paper or highlight under them. A window is moved along as the
    // highlight nears either end, far enough from each that the paper's
    // head and foot stay off screen.
    constexpr size_t kWindowFrames = 64u, kWindowEdge = 12u;
    // A live setlist's headers, its window's in order, each at most two
    // frames with its first row.
    constexpr size_t kLiveHeaders = kWindowFrames / 2u;

    struct Window
    {
        size_t first = 0u, count = 0u;  // its rows in the window
        std::vector<size_t> tiers;      // every row's tier
        std::vector<std::string> songs; // and song
        // A live setlist's: the rows it was filled from, the window's
        // headers' text, and the song the list is on with its row in the
        // window, kept there when the rows change.
        std::shared_ptr<const std::vector<setlists::LiveRow>> from;
        std::vector<std::string> headers;
        std::vector<std::string> tierNames; // every tier's header text, a live one's
        std::string at;
        size_t atRow = 0u;
    };

    // Rows from `first`, as many as fit with their headers.
    size_t windowFrom(const std::vector<size_t> &tiers, size_t first);
    // Song `song`'s row among the window's every row, none if not there.
    std::optional<size_t> rowOf(const Window &window, const std::string &song);
    // The window moved to have row `at` half way down it, on its song.
    void moveTo(Window &window, size_t at);

    // A live setlist's rows into the window, each open, and its tiers' text.
    void liveRows(Window &window, const setlists::Live &live, std::vector<bool> &active);

    // The window moved for the list's row now on, where that is near
    // either end: the row it is on now; none if it has not moved.
    std::optional<size_t> scroll(Window &window, size_t row);
    // The window moved to the nth tier's first song, or to a song picked
    // at random, never the one on: its row in the window; none if there is
    // no such tier or fewer than two songs.
    std::optional<size_t> jump(Window &window, size_t tier);
    std::optional<size_t> random(Window &window);
}
