#pragma once

// Who is on stage for each official Guitar Hero and Rock Band song, from
// the games' own song lists (assets/song_facts.tsv, tools/song_facts.py's):
// what a custom song that is one of them, a Clone Hero rip, has lost. None
// of the rips' song.ini sets vocal_gender.

#include <optional>
#include <string>

namespace gh2::custom
{
    struct SongFacts
    {
        char gender = 0;           // 'f', 'm', 'n' for no singer; 0 not known
        bool keys = false;         // a keyboard part
        std::optional<bool> fast;  // the band's clips (anim_tempo): kTempoFast, else kTempoMedium
        bool song = false;         // the song's own, not only its artist's gender
    };

    // A song's by its song.ini artist and name; else, where every song of
    // its artist's has one singer's gender, that alone.
    std::optional<SongFacts> songFacts(const std::string &artist, const std::string &title);

    // A name as the table keys it, as tools/song_facts.py's key(): ASCII,
    // accents off, lowercase, '&' as "and", a leading "the", (...), [...]
    // and anything from "feat" on left out, letters and digits only.
    std::string factKey(const std::string &name);
}
