#pragma once

// Text measured as an RndText's font sets it, to cut or break it to the
// room it has, and script's {text_fit ...} for it. On the guest thread
// only.

#include "addresses.h"

#include <cstdint>
#include <string>
#include <vector>

class PS2Runtime;
struct R5900Context;

namespace gh2::text_fit
{
    // `line` cut where it runs wider than `room`, ending in an ellipsis
    // (the font's own if it has one, else "..."), measured as RndText
    // `text`'s font sets it at `size`, in that Text's units. As it is
    // where the Text has no font yet.
    std::string fit(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t text, const std::string &line,
                    float size, float room);
    // Latin-1 text in capitals, its accented letters too.
    std::string upper(std::string latin1);

    void install(const Addresses &addresses);
}
