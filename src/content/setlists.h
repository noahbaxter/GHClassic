#pragma once

// Quickplay's setlists, one per game: the game played's own
// (content/campaigns.h), which the game builds from its campaign, and every
// other's, its songs in its tiers, shown in its own song list scene and
// unlocked as the game's own are. Which is shown changes quickplay's list
// and nothing else. The one shown and each one's song last on are kept from
// run to run (save::state).
//
//   {setlist select <name>}  TRUE if it exists and was not already shown
//   {setlist next <name>}    that, shown only once taken: a list turning
//                            away keeps the one it shows
//   {setlist take}           the next one shown, TRUE if there was one
//   {setlist look}           the next one's song list scene, else the shown one's
//   {setlist shown}          the shown one's name
//   {setlist has <name>}     TRUE if it exists
//   {setlist song [row]}     the shown one's song row last on, -1 if none;
//                            with a row, kept as that
//   {setlist scroll <row>}   the row the list is on once a setlist too long
//                            for the paper has moved its window along under
//                            it, the provider to be filled again; -1 if it
//                            has not moved
//   {setlist stale}          TRUE if a live one's rows have changed since
//                            the list last filled, a second or more ago:
//                            the provider to be filled again
//   {setlist header <n>}     the text of a live one's window's nth header
//   {setlist tiers}          how many tiers a live one has (0 for the rest)
//   {setlist tier_name <n>}  its nth tier's header text
//   {setlist tier_at}        the tier its song last on is in
//   {setlist jump <n>}       the list moved to the nth tier's first song: its
//                            row in the window, the provider to be filled
//                            again; -1 if there is no such tier
//   {setlist random}         that, to a live one's song picked at random,
//                            never the one on; -1 with fewer than two
//   {setlist frame <row>}    the paper's frame for the list's row, from the
//                            setlist's top
//   {setlist paper_base <piece> <x> <z>}  where the scene put a piece of
//                            its paper, kept from the first time
//   {setlist paper <piece> x|z}  where it goes now, for paper that goes on
//                            past the scene's (setlist_paper.cpp's kSheet)
//   {setlist paper_pose <piece> <object>}  a doodle sized and mirrored for
//                            where it goes now, before it is put there
//
// Each is named for its game: gh2, gh80s; and custom, the player's own
// songs (custom/install.h).

#include "addresses.h"
#include "formats/dtb.h"
#include "milo/milo.h"

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

class PS2Runtime;
struct R5900Context;

namespace gh2::setlists
{
    struct Tier
    {
        std::string venue;  // where its songs play when their own is locked
        std::string header; // headed by locale token song_header_<header>
        std::vector<std::string> songs;
        // A career tier: its last song is locked as its encore, where its
        // game has them (content/encores.h).
        bool encore = true;
    };

    // campaign.dta's (required_songs ...), easy first: the songs a venue
    // asks for, and the last venue's own count (GH1 keeps one for all).
    using Required = std::vector<std::array<int, 2>>;
    Required required(const dtb::Node &campaign);

    // `look` is a milo path from the ARK root (ui/sel_song_quickplay.milo).
    // Its songs' high scores start on `scoreNames`, its game's own
    // highscore_dummy_0..4, and save under `name` (save/save.h). With
    // `required`, its career's, an encore opens as that career opens it.
    void add(const std::string &name, std::vector<Tier> tiers, const std::string &look,
             const std::array<std::string, 5> &scoreNames, Required required = {});

    // One kept elsewhere and as long as it gets (custom/catalog.h): its rows
    // asked for each time the list fills, and the songs of the window it
    // fills with made ready in the config first, on the guest thread. A row
    // is its song and its tier's header text, a tier being the rows in a
    // run with the same. Its songs are not in GH2's high score table, which
    // goes in its save, whose buffer holds a few hundred songs at most
    // (save/save.cpp's kBufferSize); their bests are save.bin's [best]
    // (save/best.h). Listed only while it has a row.
    struct LiveRow
    {
        std::string song, header;
    };
    struct Live
    {
        std::function<std::shared_ptr<const std::vector<LiveRow>>()> rows;
        std::function<void(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, const std::vector<std::string> &songs)>
            ready;
        // A song's venue by GH2's name, for where it plays when its own is
        // locked (setlists.cpp's openVenue falls back from it). On the guest
        // thread.
        std::function<std::string(const std::string &song)> venue;
        // A line under each row's song name, its artist: with it, the
        // setlist shows in a copy of GH2's scene of its own, beside `look`
        // as sel_song_<name>.milo, whose rows have room for two lines
        // (content/setlist_look.h). On the guest thread.
        std::function<std::string(const std::string &song)> under;
    };
    void addLive(const std::string &name, const std::string &look, Live live);

    // That game's song list scene, served at <game>/ui/sel_song_quickplay.milo
    // and turning with the menus as the game's own does.
    void addLook(const std::string &game, milo::Dir scene);

    // That game's career list shows a song's score where it has none, as
    // a 0, as GH1's does (SongProvider::GetTextSlot, GH1 0x11a7d8, prints
    // any). GH2's leaves it blank.
    void zeroScores(const std::string &game);

    // A game's career songs: its tiers' own, without the store's.
    std::vector<std::string> careerSongs(const std::string &name);
    // How far into that setlist's career a song is, from 0 to 1: its
    // tier's place among them. None for a song in none (the store's).
    std::optional<double> careerAt(const std::string &name, const std::string &song);
    // And its store's: the bonus songs.
    std::vector<std::string> bonusSongs(const std::string &name);

    // A game that keeps its career as GH2 does, read off its disc: the
    // career's tiers (campaign.dta's order), then the store's songs, in its
    // own scene, <game>/ui/sel_song_quickplay.milo. Its songs go into the
    // config and are served from that disc, unless they are `inConfig`
    // already, as the game disc's are.
    void addDisc(const std::string &game, size_t disc, bool inConfig);

    // The setlist quickplay shows: the one picked, or after a campaign
    // switch the game now played's own.
    const std::string &shown();

    void install(PS2Runtime &runtime, const Addresses &addresses);
}
