#include "custom/cast.h"

#include "content/sources.h"

#include "custom/song_facts.h"
#include "custom/text.h"

#include <set>
#include <string_view>

namespace gh2::custom
{
    namespace
    {
        struct GenreKind
        {
            const char *name;
            std::vector<const char *> substrings; // any of these in the genre
            std::vector<const char *> wholeWords; // or any as a word of it
            std::vector<const char *> characters;
            bool grim = false; // the Grim Ripper now and then
        };

        // Gibson's, Epiphone's and Kramer's (realGuitars()).
        const std::vector<std::string> kRealGuitars = {
            "lespaul",          "sg",       "flying_v",          "explorer",   "lespaul_doublecut",
            "firebird",         "epiphone_coronet", "kramer_vanguard", "moderne", "corvus",
            "es335",            "kramer_650", "eds",             "lespaul_doublecut_sticker", "lespaul_sonex",
            "lespaul_marauder", "epiphone_casino", "epiphone_sheraton", "chet_atkins"};
        // Of those, the ones a band has from the start.
        const std::vector<std::string> kStarterGuitars = {"lespaul", "sg", "flying_v"};
        // Out of a hundred songs, how many play any guitarist rather than
        // their genre's, and in how many the Grim Ripper plays where he may.
        constexpr uint64_t kAnyIn100 = 15u, kGrimIn100 = 5u;

        // First that says wins: the heavier kinds of metal before metal,
        // punk before rock ("pop punk"), prog rock unless it says metal.
        // The guitarists as GH2 casts its own songs (punk Johnny, metal
        // Axel, Lars the death metal), by GH2's config's names.
        const std::vector<GenreKind> &kinds()
        {
            static const std::vector<GenreKind> s_kinds = {
                {"extreme",
                 {"death", "black metal", "grind", "brutal", "slam", "blackgaze", "sludge", "doom", "drone", "powerviolence"},
                 {},
                 {"deathmetal", "metal"},
                 true},
                {"metalcore",
                 {"metalcore", "post-hardcore", "post hardcore", "mathcore", "screamo", "easycore", "djent", "-core",
                  "nu metal", "nu-metal", "numetal", "rap metal"},
                 {"core", "hardcore"},
                 {"alterna", "goth", "metal"}},
                {"metal", {"metal", "thrash", "shred", "neoclassical", "speed", "nwobhm"}, {}, {"metal", "rock", "deathmetal"}, true},
                {"punk", {"punk", "skate"}, {"emo", "ska"}, {"punk", "alterna"}},
                {"game",
                 {"chip", "8-bit", "8bit", "bitpop", "video game", "videogame", "vgm", "soundtrack", "anime", "j-pop",
                  "jpop", "k-pop", "kpop", "j-rock", "jrock", "vocaloid", "touhou", "eurobeat"},
                 {"game", "ost"},
                 {"alterna", "glam", "rockabill", "goth"}},
                {"electronic",
                 {"electro", "dubstep", "edm", "house", "trance", "techno", "drum and bass", "drum & bass", "dnb",
                  "synth", "breakcore", "breakbeat", "dance", "industrial", "nightcore", "hardstyle", "disco", "ambient",
                  "glitch"},
                 {"trap"},
                 {"goth", "funk1", "alterna"}},
                {"glam", {"glam", "hair", "new wave", "80s"}, {"aor"}, {"glam"}},
                {"funk",
                 {"funk", "jazz", "fusion", "soul", "r&b", "rnb", "motown", "blues", "reggae", "latin", "hip-hop",
                  "hip hop", "hiphop", "swing", "big band"},
                 {"rap"},
                 {"funk1", "classic", "rockabill"}},
                {"alternative",
                 {"alt", "indie", "grunge", "shoegaze", "post-rock", "post rock", "math rock", "noise", "garage",
                  "britpop", "experimental", "art rock", "psych"},
                 {},
                 {"alterna", "goth", "rock"}},
                {"rock",
                 {"rock", "country", "folk", "southern", "surf", "rockabilly", "instrumental", "acoustic", "pop",
                  "classical", "orchestral", "bluegrass", "ballad", "prog"},
                 {},
                 {"rock", "classic", "rockabill", "glam"}},
            };
            return s_kinds;
        }

        const GenreKind *kindOf(const std::string &genre)
        {
            const std::string lowered = lower(genre);
            std::set<std::string> genreWords;
            std::string word;
            for (const char c : lowered + ' ')
                if (std::isalnum(static_cast<unsigned char>(c)))
                    word += c;
                else if (!word.empty())
                {
                    genreWords.insert(word);
                    word.clear();
                }
            const auto has = [&](const char *s) { return lowered.find(s) != std::string::npos; };
            const auto hasWord = [&](const char *w) { return genreWords.count(w) != 0u; };
            for (const GenreKind &kind : kinds())
            {
                if (std::string_view(kind.name) == "metalcore" && has("hardcore punk"))
                    continue;
                if (std::any_of(kind.substrings.begin(), kind.substrings.end(), has) ||
                    std::any_of(kind.wholeWords.begin(), kind.wholeWords.end(), hasWord))
                    return &kind;
            }
            return nullptr;
        }

        // GH2's career's venues in order, its last (stone) left out as its
        // own songs' casts leave it.
        const std::vector<std::string> kVenues = {"gh2_battle", "gh2_small1", "gh2_small2", "gh2_big",
                                                   "gh2_theatre", "gh2_fest", "gh2_arena"};

        // Every guitarist the kinds name, in the order they first name
        // them, each with its first outfit (config/gh2.dta's characters).
        const std::vector<std::pair<std::string, std::string>> kCharacters = {
            {"deathmetal", "deathmetal1"}, {"metal", "metal1"}, {"alterna", "alterna1"}, {"goth", "goth2"},
            {"rock", "rock2"},             {"punk", "punk1"},   {"glam", "glam1"},       {"rockabill", "rockabill1"},
            {"funk1", "funk1"},            {"classic", "classic"}};

        // A number from the seed for one thing picked (`salt`): splitmix64,
        // every bit of both stirred into every bit.
        uint64_t roll(uint32_t seed, uint32_t salt)
        {
            uint64_t x = (static_cast<uint64_t>(seed) << 32u | salt) + 0x9e3779b97f4a7c15ull;
            x = (x ^ (x >> 30u)) * 0xbf58476d1ce4e5b9ull;
            x = (x ^ (x >> 27u)) * 0x94d049bb133111ebull;
            return x ^ (x >> 31u);
        }
    }

    std::string genreKind(const std::string &genre)
    {
        const GenreKind *kind = kindOf(genre);
        return kind ? kind->name : "";
    }

    // FNV-1a.
    uint32_t seedOf(const std::string &key)
    {
        uint32_t h = 2166136261u;
        for (const unsigned char c : key)
            h = (h ^ c) * 16777619u;
        return h;
    }

    size_t pickOf(uint32_t seed, uint32_t salt, size_t n) { return n == 0u ? 0u : static_cast<size_t>(roll(seed, salt) % n); }

    size_t pickAlong(uint32_t seed, uint32_t salt, size_t n, std::optional<double> at)
    {
        if (!at || n == 0u)
            return pickOf(seed, salt, n);
        constexpr size_t kSteps = 6u;
        const size_t step = std::min(static_cast<size_t>(std::clamp(*at, 0.0, 1.0) * kSteps), kSteps - 1u);
        const size_t from = step * n / kSteps, to = std::max((step + 1u) * n / kSteps, from + 1u);
        return from + static_cast<size_t>(roll(seed, salt) % (to - from));
    }

    const std::vector<std::string> &realGuitars() { return kRealGuitars; }

    Cast castOf(const Tags &tags)
    {
        Cast out;
        out.seed = seedOf(factKey(tag(tags, "artist").value_or("")) + "|" + factKey(tag(tags, "name").value_or("")));
        const auto pick = [&](const std::vector<std::string> &from, uint32_t salt) -> const std::string &
        { return from[pickOf(out.seed, salt, from.size())]; };

        // Its genre's guitarists, now and then any; every one for none.
        const GenreKind *kind = kindOf(tag(tags, "genre").value_or(""));
        const std::string character =
            kind && roll(out.seed, kSaltAnyCharacter) % 100u >= kAnyIn100
                ? kind->characters[pickOf(out.seed, kSaltCharacter, kind->characters.size())]
                : kCharacters[pickOf(out.seed, kSaltCharacter, kCharacters.size())].first;
        out.outfit = sources::id(
            "gh2", std::find_if(kCharacters.begin(), kCharacters.end(), [&](const auto &c) { return c.first == character; })->second);
        out.character =
            sources::id("gh2", (!kind || kind->grim) && roll(out.seed, kSaltGrim) % 100u < kGrimIn100 ? "grim" : character);
        out.realGuitar = pick(kRealGuitars, kSaltGuitar);
        out.guitar = pick(kStarterGuitars, kSaltStarterGuitar);

        // song.ini's difficulty, 0 to 6, its venue moved up to two either
        // way; with none, any.
        long diff = number(tags, "diff_guitar").value_or(-1);
        if (diff < 0)
            diff = number(tags, "diff_band").value_or(-1);
        const long last = static_cast<long>(kVenues.size()) - 1;
        if (diff < 0)
            out.venue = pick(kVenues, kSaltVenue);
        else
        {
            const long shift = static_cast<long>(roll(out.seed, kSaltVenue) % 5u) - 2;
            out.venue = kVenues[static_cast<size_t>(std::clamp(std::min(diff, last) + shift, 0L, last))];
        }
        return out;
    }
}
