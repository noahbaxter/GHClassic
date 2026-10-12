#pragma once

#include <string>

namespace gh2
{
    namespace gh1
    {
        // A GH1 guitarist, as one more outfit of a GH2 character.
        struct Guitarist
        {
            const char *character; // in config's (characters ...)
            const char *label;     // the outfit picker's (sel_character.dta)
            const char *base;      // GH2's outfit it sits beside, here grafted onto
            const char *folder;    // GH1's, charsys/<folder>/
            bool doorOpens;        // the picker door stands open behind them

            std::string name() const { return std::string(character) + "gh1"; } // ours
            // GH1's clip names and some files go by the folder's first word:
            // hair_idle_ui for hair_metal, nu for nu_metal.
            std::string prefix() const
            {
                const std::string f = folder;
                return f.substr(0, f.find('_'));
            }
        };

        // GH1's locale names the folders: hair_metal Izzy, nu_metal Pandora,
        // hiphop Xavier. GH1's punk and hiphop idle leaning on the back wall,
        // where the door opens, so it stays shut behind them.
        inline constexpr Guitarist kGuitarists[] = {
            // character  label          base        folder        door opens
            {"punk",      "SAFETY PINS", "punk1",    "punk",       false},
            {"alterna",   "CORSET",      "alterna1", "alterna",    true},
            {"metal",     "SHORT",       "metal1",   "metal",      true},
            {"glam",      "BIGGER BOOT", "glam1",    "hair_metal", true},
            {"goth",      "RAZORS",      "goth2",    "nu_metal",   true},
            {"funk1",     "JADE",        "funk1",    "hiphop",     false},
            {"classic",   "BRIT",        "classic",  "classic",    true},
            {"grim",      "SCHEMIN'",    "grim",     "grim",       true},
        };
    }
}
