// Rocks the 80s: a campaign of its own, and beside another game's its songs
// as a quickplay setlist and its guitarists as outfits.
//
// The 80s guitarists are GH2's characters in new outfits, built on the same
// rigs (every anim but goth1_main is byte-identical), and they sit at GH2's
// outfit paths. Each goes in under a name of its own as one more outfit of
// its character, served from the 80s disc and playing GH2's anims.
//
// An outfit loads char/<outfit>/og/<outfit>.milo (AddLoadChar, 0x128778),
// with _ui and _horse beside it.

#include "gh80s/install.h"

#include "disc/ark.h"
#include "content/card.h"
#include "content/games.h"
#include "content/outfits.h"
#include "content/campaigns.h"
#include "content/setlists.h"

#include <string>

namespace gh2
{
    namespace
    {
        struct Outfit
        {
            const char *character; // in config's (characters ...)
            const char *label;     // the outfit picker's (sel_character.dta)
            const char *base;      // GH2's outfit it sits beside, here replaced
            const char *name;      // ours
        };

        // The 80s disc names its outfits as GH2 does the ones they replaced,
        // so base is also the folder and file stem it is read from.
        constexpr Outfit kOutfits[] = {
            // character  label          base        name
            {"punk",      "SIDE SWEEP",  "punk1",    "punk3"},
            {"alterna",   "LACE",        "alterna1", "alterna3"},
            {"metal",     "JACKET",      "metal1",   "metal3"},
            {"glam",      "BANDANAS",    "glam1",    "glam3"},
            {"goth",      "BLAZERS",     "goth2",    "goth3"},
            {"grim",      "WATCHIN'",    "grim",     "grim2"},
        };

    }

    void installEighties()
    {
        const auto disc = games::disc("gh80s");
        if (!disc)
            return;
        // The 80s is GH2's code over its own files at GH2's paths, so with
        // its archive in front it is the whole game (content/campaigns.h).
        campaigns::add("gh80s", ark::addLayer(*disc));
        setlists::addDisc("gh80s", *disc, false);
        // GH2's save code at GH2's addresses, its own folder: a 0x29c00-byte
        // save (GHMCSaveData 0x14b278), title 0x404918 broken after 13
        // characters (SetupMCIcon 0x14b178).
        content::addCardGame(*disc, "gh80s", false, 0x29c00u, "Guitar Hero: Rocks the 80s", 13);
        for (const Outfit &outfit : kOutfits)
        {
            const std::string base = outfit.base, name = outfit.name;
            ark::rename("char/" + name + "/og/gen/" + name, *disc, "char/" + base + "/og/gen/" + base);
            // The highway (track/surfaces/%s_keep.bmp) and photos go by
            // outfit too.
            ark::rename("track/surfaces/gen/" + name + "_keep", *disc, "track/surfaces/gen/" + base + "_keep");
            outfits::photosFrom(name, *disc, base);
            outfits::add("gh80s", base, outfit.character, name, base, outfit.label);
        }
    }
}
