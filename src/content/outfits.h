#pragma once

// Outfits beyond config's own, for each character's outfit picker.

#include "addresses.h"
#include "milo/milo.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

class PS2Runtime;

namespace gh2::outfits
{
    // One more outfit of `character`, by its id (content/sources.h), after
    // its own in config's (characters ...): unlocked, as an outfit the store
    // doesn't sell is, and cached by the previews wherever `beside` is.
    // `label` is the picker's (sel_character.dta); a one-outfit character's
    // own outfit, which the locale never named, gets one too. The caller
    // serves its og/gen files; this serves them again playing beside's
    // anims. It is there while another game than its own (its source) is
    // played (content/campaigns.h), outside the career, once its own game's
    // career has it ({campaigns owns}).
    void add(const std::string &outfit, const std::string &character, const std::string &beside,
             const std::string &label);

    // Each outfit added, as add() was given it, with its game.
    struct Outfit
    {
        std::string game, outfit, character, beside, label;
    };
    const std::vector<Outfit> &added();

    // The config and the screens are another game's now: the outfits go in
    // again as its profiles and previews are made.
    void campaignChanged();

    // The added outfit's own portrait in quickplay's picker, made from GH2's
    // char_<character>.tex when the picker first loads: portrait_<outfit>.mat
    // over portrait_<outfit>.tex, beside GH2's own (sel_character.dta).
    // GH2's art is grey, inked in the Mat's black; `colour` art is drawn as is.
    void portrait(const std::string &outfit, bool colour,
                  std::function<std::vector<uint8_t>(const std::vector<uint8_t> &tex)> make);
    // Something more in quickplay's picker scene, `edit` run on it after the
    // portraits go in. False leaves it out.
    void pickerScene(std::function<bool(milo::Dir &scene)> edit);

    void install(PS2Runtime &runtime, const Addresses &addresses);
}
