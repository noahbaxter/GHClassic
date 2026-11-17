#pragma once

// The boot's first picture kept for the next boot, which shows it from the
// moment its window opens until the game draws its own: the save check's
// dialog with fast_boot, the first logo without. The game's own picture is
// seconds away (the host builds GH1's outfits and more before the game
// starts), and the window sat black for them. Kept under the user data
// directory's cache, by the things that change what that picture is: the
// game whose menus open first, the aspect and the discs. None kept, or kept
// for other ones: black, as before, the first boot alone.

#include <cstdint>
#include <optional>
#include <vector>

namespace gh2::boot_picture
{
    struct Picture
    {
        uint32_t width = 0, height = 0;
        float aspect = 4.0f / 3.0f; // shown at
        std::vector<uint8_t> rgba;
    };

    // The one kept for this boot's game, aspect and discs.
    std::optional<Picture> kept();

    // Kept for the next boot, written on a thread of its own.
    void keep(Picture picture);
}
