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
    }
}
