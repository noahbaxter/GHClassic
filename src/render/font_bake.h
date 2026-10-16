#pragma once

// The traced fonts (render/vector_fonts.h) drawn into the game's own font
// sheets wherever a character's cell holds some other art, at the sheet's
// resolution and in its palette, so text has every glyph its Font lists with
// HD fonts off too. GH2's HUD rokk lists 0 to 9 and has their cells, but its
// sheet draws Ü ¡ Ó Í Ç where 5 to 9 belong.

#include "milo/objects.h"

#include <functional>
#include <optional>
#include <utility>

namespace gh2
{
    // After ark::install, which indexes the discs.
    void installFontBake();

    // Where on a packed Font's sheet, W by H pixels, a box w by h fits clear
    // of every character's box in its info (and, with `inked`, of the
    // sheet's own ink): its top left. None when there is no room.
    std::optional<std::pair<int, int>> freeBox(const milo::Bytes &body, const milo::Font &font, int W, int H, int w, int h,
                                               const std::function<bool(int, int)> &inked = {});
}
