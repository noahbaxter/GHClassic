#pragma once

#include <cstdint>
#include <vector>

namespace gh2::movie
{
    // A decoded movie frame, RGBA8, as the IPU would have converted it.
    struct Picture
    {
        uint64_t serial = 0;
        uint32_t width = 0;
        uint32_t height = 0;
        std::vector<uint8_t> rgba;
    };

    // The player's side: the newest picture to show in place of the game's
    // frames, and when the movie is over, nothing.
    void showPicture(Picture &picture);
    void endPictures();

    // The frontend's side: true while a movie has a picture up, with `out`
    // brought up to the newest when it is behind.
    bool latestPicture(Picture &out);
}
