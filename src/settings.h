#pragma once

// The player's settings, one table entry each: the entry drives
// settings.ini, and later the overlay row and the "PS2 mode" preset.

#include <string>

namespace gh2::settings
{
    enum Key
    {
        kWidescreen,
        kFrameRate, // game frames per second, 0 for the display's rate
        kMipmaps,   // smaller copies for textures the game shipped without
        kVideoLagMs, // how late the picture reaches the player
        kAudioLagMs, // how late the sound does
        kKeyCount,
    };

    int get(Key key);
    // Whether settings.ini holds the key, rather than it taking its default.
    bool isSet(Key key);
    // Snapped to the entry's step and range; written to settings.ini at once.
    void set(Key key, int value);

    // Read and write `path` in place of the user data directory's
    // settings.ini. Before the first get or set.
    void usePath(const std::string &path);

    // `file` in the user data directory, which is made if missing.
    std::string userDataPath(const std::string &file);
}
