#pragma once

// Outfits beyond config's own, for each character's outfit picker.

#include "addresses.h"

#include <string>

class PS2Runtime;

namespace gh2::outfits
{
    // One more outfit of `character`, after its own in config's
    // (characters ...): unlocked, as an outfit the store doesn't sell is,
    // and cached by the previews wherever `beside` is. `label` is the
    // picker's (sel_character.dta); a one-outfit character's own outfit,
    // which the locale never named, gets one too. The caller serves its
    // og/gen files; this serves them again playing beside's anims.
    void add(const std::string &character, const std::string &outfit, const std::string &beside,
             const std::string &label);

    // The outfit's photos (ui/image/og/photo_%s%i_keep.bmp, by outfit and
    // index) as source's on that disc.
    void photosFrom(const std::string &outfit, size_t disc, const std::string &source);

    void install(PS2Runtime &runtime, const Addresses &addresses);
}
