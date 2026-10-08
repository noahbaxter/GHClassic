#pragma once

// The few fields of Tex, Mat and Font bodies the host reads, in a v24
// (PS2) or v25 (360) file. Each starts with its revision and an Hmx::Object
// block: revision 0 on PS2, 1 with a note on 360.

#include "milo/milo.h"

namespace gh2::milo
{
    // A Tex body, revision 10 on both: Object, width, height and bits per
    // pixel as words, the path of the picture it was made from, a mip bias,
    // its type, a byte, then its bitmap (formats/bitmap.h) when it has one.
    struct Tex
    {
        std::string source;  // empty for one filled at run time
        uint32_t type = 0;   // milo::tex's type bits (layout.h)
        size_t bpp = 0;      // where its bits per pixel word is
        size_t bitmap = 0;   // where its bitmap is; the body's size with none
    };
    std::optional<Tex> readTex(const Bytes &body, uint32_t version);

    // The Mat a Font body (revision 15) draws its glyphs with.
    std::optional<std::string> fontMat(const Bytes &body, uint32_t version);

    // The Tex a Mat body (revision 27 on PS2, 28 on 360) has as its diffuse.
    std::optional<std::string> matDiffuse(const Bytes &body, uint32_t version);
}
