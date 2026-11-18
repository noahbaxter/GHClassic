#pragma once

// A custom song's quickplay cast, as GH2's songs each name theirs
// (songs.dta's (quickplay (character_outfit) (guitar) (venue))). Each pick
// is the song's own, rolled from one seed off its song.ini's artist and
// name, so the same song has the same cast everywhere:
//
//   a guitarist among its genre's characters, now and then any, and now
//   and then the Grim Ripper where its genre is a heavy one or none;
//   any of the real guitars;
//   the venue as far along GH2's career as its song.ini difficulty, moved
//   up to two either way.
//
// songs.dta has the guitarist's first outfit and a Les Paul, SG or Flying
// V: there from the start. With song defaults on, quickplay plays an
// outfit of that guitarist's (any game's) and that guitar in a finish,
// each picked by the song, where unlocked (content/quickplay.cpp). A song
// whose venue is locked plays in the nearest open one before it
// (content/setlists.h's Live).

#include "custom/song_ini.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace gh2::custom
{
    struct Cast
    {
        std::string character; // its guitarist, by GH2's config's name ("grim" for the Grim Ripper)
        std::string outfit;    // songs.dta's: a GH2 guitarist's first outfit, never the Grim Ripper's
        std::string guitar;    // songs.dta's: a Les Paul, SG or Flying V
        std::string realGuitar; // the guitar it plays once unlocked: any real one
        std::string venue;
        uint32_t seed = 0u; // its picks' seed
    };
    Cast castOf(const Tags &tags);

    // Each thing picked from a song's seed rolls with its own salt.
    enum Salt : uint32_t
    {
        kSaltCharacter = 1,
        kSaltGuitar = 2,
        kSaltVenue = 3,
        kSaltFinish = 4,
        kSaltAnyCharacter = 5,
        kSaltGrim = 6,
        kSaltOutfit = 7,
        kSaltStarterGuitar = 8,
    };

    // A seed from a key, and which of `n` things it picks with a salt,
    // spread evenly over them. 0 for none.
    uint32_t seedOf(const std::string &key);
    size_t pickOf(uint32_t seed, uint32_t salt, size_t n);
    // Which of `n` things, plainest first, a song `at` (0 to 1) along a
    // career picks: the list cut into six steps, rounded down, the song's
    // step its slice and the seed its pick in it; any where it has no place.
    size_t pickAlong(uint32_t seed, uint32_t salt, size_t n, std::optional<double> at);

    // The real guitars a store sells, by GH2's config's names: excludes
    // prize, cheat and made-up guitars.
    const std::vector<std::string> &realGuitars();

    // A genre as one of a few kinds: "metal", "punk", "rock" and the like
    // by the words in it, "" for none that says.
    std::string genreKind(const std::string &genre);
}
