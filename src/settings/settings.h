#pragma once

// The player's settings, one table entry each: the entry drives
// settings.ini, and later the overlay row and the "PS2 mode" preset.

#include <string>

namespace gh2::settings
{
    enum Key
    {
        kWidescreen,
        kFrameRateCap, // game frames per second, 0 for the display's rate
        kMipmaps,   // smaller copies for textures the game shipped without
        kMsaa,      // samples per pixel
        kFrustumCull, // nothing drawn for what its sphere puts off the picture
        kVideoLagMs, // how late the picture reaches the player
        kAudioLagMs, // how late the sound does
        kBandVolume, // the game's own volumes, 0 to 11
        kGuitarVolume,
        kFxVolume,
        kStereo,
        kFastBoot,   // straight to the main menu, past the logos and splash
        kLefty,      // player 1's frets mirrored
        kLeftyP2,
        kExportCard, // each save also written to GHClassic.ps2, a PCSX2 card
        kTrackOnAtStart, // the player's track heard before its first hit
        kSustainReleaseMs, // a sustain let go this close to its end stays heard
        kWhammyHold, // a whammy bar held down bends every sustain
        kDiscPrefer, // kChd or kIso: the image taken when the disc folder has both
        kCheckUpdates, // a newer release offered when the game starts
        kKeyCount,
    };

    enum DiscFormat
    {
        kChd,
        kIso,
    };

    int get(Key key);
    // Whether settings.ini holds the key, rather than it taking its default.
    bool isSet(Key key);
    // Snapped to the entry's step and range; written to settings.ini at once.
    void set(Key key, int value);

    // Read and write `path` in place of the user data directory's
    // settings.ini. Before the first get or set.
    void usePath(const std::string &path);

    // Keep everything in `dir` in place of the user data directory: saves,
    // settings, bindings, cards and the log. "stable" is the stable track's
    // own, which a development build of either track can share. Before the
    // first userDataPath.
    void useDataDir(const std::string &dir);

    // `file` in the user data directory, which is made if missing.
    std::string userDataPath(const std::string &file);
}
