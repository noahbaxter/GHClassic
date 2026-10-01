// The 80s guitarists are GH2's characters in new outfits, built on the same
// rigs (every anim but goth1_main is byte-identical), and they sit at GH2's
// outfit paths. Each goes in under a name of its own as one more outfit of
// its character, served from the 80s disc and playing GH2's anims.
//
// An outfit loads char/<outfit>/og/<outfit>.milo (AddLoadChar, 0x128778),
// with _ui and _horse beside it.

#include "eighties.h"

#include "disc/ark.h"
#include "outfits.h"

#include <string>

namespace gh2
{
    namespace
    {
        struct Outfit
        {
            const char *character; // in config's (characters ...)
            const char *name;      // ours
            const char *source;    // its folder and file stem on the 80s disc
            const char *label;     // the outfit picker's (sel_character.dta)
        };

        // The 80s disc names its outfits as GH2 does the ones they replaced.
        constexpr Outfit kOutfits[] = {
            {"punk", "punk3", "punk1", "'80S MOHAWK"},     {"alterna", "alterna3", "alterna1", "'80S SKULLS"},
            {"glam", "glam3", "glam1", "'80S CODPIECE"},   {"goth", "goth3", "goth2", "'80S LEATHERS"},
            {"metal", "metal3", "metal1", "'80S SHIRT"},   {"grim", "grim2", "grim", "'80S"},
        };
    }

    void installEighties()
    {
        const auto disc = ark::discWithSerial("SLUS_215.86");
        if (!disc)
            return;
        for (const Outfit &outfit : kOutfits)
        {
            const std::string name = outfit.name;
            const std::string source = outfit.source;
            ark::rename("char/" + name + "/og/gen/" + name, *disc, "char/" + source + "/og/gen/" + source);
            // The 80s outfit replaced GH2's default one, so source also names
            // the outfit it is cached beside.
            outfits::add(outfit.character, name, source, outfit.label);
        }
        // Grim's one outfit has no name in the locale: his picker never
        // showed before he had two.
        outfits::label("grim", "grim", "CLASSIC");
    }
}
