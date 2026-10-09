#pragma once

// The player's settings. config/settings.template.ini, compiled in, holds
// each one's default, section and words, and settings.ini is written from it;
// a table entry here holds its kind and what the PS2 did, for the overlay row
// and the "PS2 mode" preset later.

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
        kBand,       // whose backing band plays: the venue's game's, or one game's everywhere
        kDepthOfField, // the PS2's blur behind what a camera shot looks at
        kBandShadows,  // the band's shadows in GH1's venues: none, the room's as GH1 drew them, or GH2's own
        kSongDefaults, // quickplay plays a song as its own game's guitarist, guitar and venue, unpicked
        kInSongUi,     // outside a career, whose loading, pause, fail, results, HUD and tips a song has
        kGems,         // outside a career, whose gems: the in-song look's, or one game's
        kMenuMusic,    // outside a career, whose menu loops: the theme's, one game's, or every game's
        kMenuMusicOrder, // the loops one after another in order, one over and over, or shuffled
        kTextures,  // kPs2 or kX360: whose textures the track, HUD and menus draw
        kModels,    // kPs2 or kX360: whose venues, characters and guitars load
        kShading,   // kPs2 or kX360: whose shading and post the renderer draws with
        kKeyCount,
    };

    enum MenuMusic
    {
        kMusicTheme,
        kMusicGh1,
        kMusicGh2,
        kMusicGh80s,
        kMusicAll,
    };

    enum MenuMusicOrder
    {
        kOrderPlaylist,
        kOrderLoop,
        kOrderShuffle,
    };

    enum InSongUi
    {
        kInSongVenue,
        kInSongGh1,
        kInSongGh2,
        kInSongGh80s,
    };

    enum Gems
    {
        kGemsAuto,
        kGemsGh1,
        kGemsGh2,
    };

    enum BandShadows
    {
        kShadowsOff,
        kShadowsClassic,
        kShadowsGh2,
    };

    enum DiscFormat
    {
        kChd,
        kIso,
    };

    enum Band
    {
        kBandVenue,
        kBandGh2,
        kBandGh1,
    };

    // Which GH2's assets are drawn.
    enum AssetSet
    {
        kPs2,
        kX360, // the 360 disc's, while it is mounted
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
