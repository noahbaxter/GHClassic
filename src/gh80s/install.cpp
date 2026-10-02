// The 80s guitarists are GH2's characters in new outfits, built on the same
// rigs (every anim but goth1_main is byte-identical), and they sit at GH2's
// outfit paths. Each goes in under a name of its own as one more outfit of
// its character, served from the 80s disc and playing GH2's anims.
//
// An outfit loads char/<outfit>/og/<outfit>.milo (AddLoadChar, 0x128778),
// with _ui and _horse beside it.

#include "gh80s/install.h"

#include "disc/ark.h"
#include "content/outfits.h"

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
        const auto disc = ark::discWithSerial("SLUS_215.86");
        if (!disc)
            return;
        for (const Outfit &outfit : kOutfits)
        {
            const std::string base = outfit.base, name = outfit.name;
            ark::rename("char/" + name + "/og/gen/" + name, *disc, "char/" + base + "/og/gen/" + base);
            // The highway (track/surfaces/%s_keep.bmp) and photos go by
            // outfit too.
            ark::rename("track/surfaces/gen/" + name + "_keep", *disc, "track/surfaces/gen/" + base + "_keep");
            outfits::photosFrom(name, *disc, base);
            outfits::add(outfit.character, name, base, outfit.label);
        }
    }
}
