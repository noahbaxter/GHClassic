// Rocks the 80s: a campaign of its own, and beside another game's its songs
// as a quickplay setlist and its guitarists as outfits.
//
// The 80s guitarists are GH2's characters in new outfits, built on the same
// rigs (every anim but goth1_main is byte-identical), at GH2's outfit paths
// on its disc: gh80s_punk1 is its char/punk1/ (content/sources.h). Each is
// one more outfit of its GH2 character, playing GH2's anims.

#include "gh80s/install.h"

#include "disc/ark.h"
#include "content/card.h"
#include "content/games.h"
#include "content/hidden_guitars.h"
#include "content/outfits.h"
#include "content/campaigns.h"
#include "content/setlists.h"
#include "content/sources.h"
#include "milo/milo.h"

#include <string>

namespace gh2
{
    namespace
    {
        struct Outfit
        {
            const char *character; // in config's (characters ...)
            const char *label;     // the outfit picker's (sel_character.dta)
            const char *name;      // on both discs: the 80s' and GH2's it sits beside
        };

        constexpr Outfit kOutfits[] = {
            // character  label          name
            {"punk",      "SIDE SWEEP",  "punk1"},
            {"alterna",   "LACE",        "alterna1"},
            {"metal",     "JACKET",      "metal1"},
            {"glam",      "BANDANAS",    "glam1"},
            {"goth",      "BLAZERS",     "goth2"},
            {"grim",      "WATCHIN'",    "grim"},
        };

    }

    void installEighties()
    {
        const auto disc = games::disc("gh80s");
        if (!disc)
            return;
        // Before its config is read: its songs by our names.
        sources::addDisc("gh80s", *disc);
        // The 80s is GH2's code over its own files at GH2's paths, so with
        // its archive in front it is the whole game (content/campaigns.h).
        // But for its intro movie, which is on the disc beside the archive.
        const size_t layer = ark::addLayer(*disc);
        ark::lendFromDisc(layer, "videos/intro.pss", *disc, "VIDEOS/INTRO.PSS");
        stockStore("gh80s", *disc, layer);
        campaigns::add("gh80s", layer);
        setlists::addDisc("gh80s", *disc, false);
        // GH2's save code at GH2's addresses, its own folder: a 0x29c00-byte
        // save (GHMCSaveData 0x14b278), title 0x404918 broken after 13
        // characters (SetupMCIcon 0x14b178).
        content::addCardGame(*disc, "gh80s", false, 0x29c00u, "Guitar Hero: Rocks the 80s", 13);
        for (const Outfit &outfit : kOutfits)
        {
            const std::string name = sources::id("gh80s", outfit.name);
            outfits::add(name, sources::id("gh2", outfit.character), sources::id("gh2", outfit.name), outfit.label);
            // The 80s' picker draws each of its six as GH2's did, its own art.
            const std::string tex = std::string("char_") + outfit.character + ".tex";
            outfits::portrait(name, false, [disc = *disc, tex](const std::vector<uint8_t> &)
                              {
                                  const auto file = ark::readFile(disc, "ui/gen/sel_character.milo_ps2");
                                  const auto scene = file ? milo::read(*file) : std::nullopt;
                                  const auto i = scene ? milo::find(*scene, tex) : std::nullopt;
                                  return i ? scene->bodies[*i] : std::vector<uint8_t>();
                              });
        }
    }
}
