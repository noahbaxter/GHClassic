#pragma once

// Another game's save folder on a PCSX2 card (save/save.h), read off its disc.

#include <cstddef>
#include <cstdint>
#include <string>

namespace gh2::content
{
    // Its folder and icon from the disc's config/mc.dta and mc/<file>; `title`
    // and `lineBreak` are what that game gives MCSetPS2IconTitle.
    void addCardGame(size_t disc, const std::string &game, bool gh1Layout, uint32_t dataSize, const std::string &title,
                     int lineBreak);
}
