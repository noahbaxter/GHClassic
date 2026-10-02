#pragma once

#include <string>

namespace gh2
{
    namespace gh1
    {
        // A GH1 guitarist, as one more outfit of a GH2 character. GH1 names
        // most of its files after the folder; the rest are spelled out.
        struct Guitarist
        {
            const char *gh1Folder;           // charsys/<gh1Folder>/
            const char *gh2Character;        // in config's (characters ...)
            const char *baseOutfit;          // GH2's, the GH1 model is grafted onto
            const char *label;               // the outfit picker's (sel_character.dta)
            const char *clipPrefix = nullptr; // GH1's clip names, <clipPrefix>_idle_ui
            const char *highway = nullptr;   // GH1's, track/surfaces/<highway>.bmp
            const char *faceFile = nullptr;  // GH1's face scene, charsys/<gh1Folder>/<faceFile>.rnd
            bool pickerDoorOpens = true;     // the picker door swings open behind them

            std::string outfit() const { return std::string(gh2Character) + "gh1"; }
            std::string clips() const { return clipPrefix ? clipPrefix : gh1Folder; }
            std::string track() const { return highway ? highway : gh1Folder; }
            std::string face() const { return faceFile ? faceFile : std::string(gh1Folder) + "_face"; }
        };
    }
}
