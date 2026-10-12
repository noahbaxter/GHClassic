#pragma once

// The player's settings. config/settings.template.ini, compiled in, holds
// each one's default, section and words, and settings.ini is written from it;
// a table entry here holds its kind and what the PS2 did, for the overlay row
// and the "PS2 mode" preset later.

#include <functional>
#include <string>

namespace gh2::settings
{
    enum Key
    {
        kWidescreen,
        kFrameRateCap, // game frames per second, 0 for the display's rate
        kMipmaps,   // smaller copies for textures the game shipped without
        kMsaa,      // samples per pixel
        kRenderScale, // drawn at this percent of the window's size each way, then brought to it
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
        kTrueLeftyFlip, // a lefty player's guitarist plays left-handed too
        kExportCard, // each save also written to GHClassic.ps2, a PCSX2 card
        kTrackOnAtStart, // the player's track heard before its first hit
        kSustainReleaseMs, // a sustain let go this close to its end stays heard
        kWhammyHold, // a whammy bar held down bends every sustain
        kNoteSpeedEasy, // Clone Hero's note speed in tenths, on easy and medium
        kNoteSpeedHard, // and on hard and expert
        kHighwayLength, // how far up the track a gem first shows: the game's 110
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
        kVenues,     // outside a career, kSd or kHd: GH2's venues as the PS2 has them, or the 360's
        kCharacters, // its guitarists and band
        kGuitars,
        kHighway,    // its track and fretboards: the venues' choice, or one of its own
        kFrameLog,   // frametimes.csv: each presented frame's times and what it did
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
        kBandGh2x, // GH2's members as the 360 has them, whatever the graphics
    };

    // A pick of the pickers', kept here: the PS2's, or the 360 disc's.
    enum Models
    {
        kSd,
        kHd,
    };

    enum Highway
    {
        kHighwayAuto, // as kVenues, where the venue is one of GH2's
        kHighwayClassic,
        kHighwayHd,
    };

    // What the 360 disc has its own of.
    enum Look
    {
        kLookVenue,      // GH2's venues and their crowd
        kLookCharacters, // its guitarists and band
        kLookGuitars,
        kLookHighway,    // its track and fretboards
        kLookUiArt,      // its HUD and menu pictures
        kLookLighting,   // its shading, shadows, bloom, colour and focus blur
    };

    // What is played now, for hd.
    struct Playing
    {
        bool mounted = false;  // the 360 disc is
        bool career = false;   // in a career, which is all its own game's
        bool nextGen = false;  // that career is the 360's
        bool gh2Venue = false; // the venue a song plays in is one of GH2's
        bool inVenue = false;  // a venue is drawn
    };

    int get(Key key);
    // Whether that is the 360's now. A career is its own game's all through.
    // Outside one the models are the pickers' three, the lighting is the
    // venue's (the guitarist's or guitar's on a screen with no venue), and
    // the HUD's and menus' pictures, the same ones drawn larger, are the
    // 360's wherever its disc is mounted. It can change from one song to
    // the next.
    bool hd(Look look);
    // Who says what is played. Until one is given, nothing is the 360's.
    void setPlaying(std::function<Playing()> now);
    // Whether settings.ini holds the key, rather than it taking its default.
    bool isSet(Key key);
    // Snapped to the entry's step and range; written to settings.ini at once.
    void set(Key key, int value);
    // The pickers' three, kHighway and kBand taken again from settings.ini
    // when it has been written since last asked: one edited while the game
    // runs counts from the next thing loaded.
    void rereadGraphics();

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

    // `file` beside settings.ini, wherever usePath put that.
    std::string besideSettings(const std::string &file);
}
