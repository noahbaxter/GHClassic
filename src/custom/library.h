#pragma once

// Custom songs on disk: every song folder (song.ini beside notes.mid or
// notes.chart, and its audio) and .sng package (formats/sng.h, a folder in
// one file; its path is the song's folder) under the chart folders the player names in
// settings.ini, and under our own songs/ folder. Chart folders are only
// read. What we write is in our own folder (custom/index.h).
//
// A large library takes minutes only to walk (18,000 songs on a network
// drive, ten), so a start reads the index the last scan left, and a scan
// runs behind the game: what it finds is there from the next start, or
// this one's when it is quick (custom/install.cpp).

#include "custom/song_ini.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace gh2::custom
{
    // Song::chartHas bits.
    constexpr uint8_t kHasVocals = 1u, kHasKeys = 2u, kHasDrums = 4u;

    // Why a song does not play, as badsongs.txt groups them.
    enum class Reject : uint8_t
    {
        None,
        NoChart,
        BadChart,
        NoPart,
        NoAudio,
        BadAudio,
        BadPackage,
    };

    struct Song
    {
        std::filesystem::path dir;
        std::string chart; // notes.mid or notes.chart
        int64_t iniTime = 0, chartTime = 0; // when each was written, as the index saw it
        std::string chartMd5; // its chart file's (formats/md5.h): a copy's is the same
        std::string why; // empty if it plays, else why not
        Reject reject = Reject::None; // why's kind
        uint8_t parts = 0u; // bit n Part n, those with gems
        uint16_t charted = 0u; // bit 4 * part + difficulty, easy 0: those with gems
        uint8_t chartHas = 0u; // kHasVocals, kHasKeys, kHasDrums: for who is on stage
        // Its lead part's beats a minute from its first note to its last,
        // as its band plays it (custom/stage/stage.h: halved past 210, doubled
        // under 80); 0 not known.
        float bandBpm = 0.0f;
        std::vector<std::string> stems; // its audio's file names
        std::string art; // its album art's file name, empty for none
        std::vector<std::string> sections; // the chart's, for practice's names
        double length = 0.0; // seconds to its last note's end
        Tags tags;
    };

    // Every song folder under `roots`, read again where its files changed
    // since `known` saw them, `workers` folders at once. Each is given to
    // `each` as it is found, on the worker that found it.
    std::vector<Song> scan(const std::vector<std::filesystem::path> &roots, const std::vector<Song> &known,
                           unsigned workers = 32u, const std::function<void(const Song &)> &each = {});

    // One folder's or .sng package's song, read whole (tests only; the game
    // reads songs through scan).
    Song read(const std::filesystem::path &dir, const std::string &chart);
    bool isPackage(const std::filesystem::path &path);
    // A song's file by the name the song keeps for it (Song's names are
    // UTF-8), in its folder or package.
    std::filesystem::path songPath(const Song &song, const std::string &name);
    // A song's file, `path` its folder or package and its name: from the
    // folder, or out of the package. Empty if there is none.
    std::string songFile(const std::filesystem::path &path);
    // Whether a stem file is that part's ("guitar", "drums"): its own, or a
    // split of it (drums_2, vocals_1_explicit), in any case.
    bool partStem(const std::string &file, const std::string &part);

    // Who stands at the front of its band: nullptr for GH2's default band's
    // singer (gh2.dta's default_band), "" for no one (an instrumental, as
    // Soy Bomb), else "female_singer" (as Rock Band's) or, with no one
    // singing, a keys part's "metal_keyboard" (as GH2's Jessica and YYZ).
    // GH2 has no song with both a singer and a keyboard player: one stands in
    // the other's place. An official song's is its game's (custom/
    // song_facts.h), its singer's gender song.ini's vocal_gender where it
    // has one. Another's is sung where its chart or stems say, and an
    // instrumental only where its stems are split down to the drums with no
    // singing among them and its chart has drums and bass but no singing: a
    // chart without a vocal part is most often one without its vocals
    // charted.
    const char *bandFront(const Song &song);
    // Whether its band plays GH2's fast clips (anim_tempo kTempoFast): an
    // official song's as its game has it, else from 120 BPM, as most of
    // GH2's own are.
    bool bandFast(const Song &song);

    // Of songs that play with the same chart, the one listed: the first by
    // folder, so the same each scan.
    bool listedBefore(const Song &a, const Song &b);
    // Each song that plays but is another's copy, by folder, to the folder
    // of the one listed.
    std::map<std::filesystem::path, std::filesystem::path> copies(const std::vector<Song> &songs);
}
