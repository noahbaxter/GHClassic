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

    // A Font body past its Mat: a glyph's cell in its sheet's pixels, then
    // everything after (its characters, their widths and kerning). Then
    // where, in the body, RndFont::Load (0x1fa9f0) reads what follows: after
    // a size and a word the characters, the kerning, the texture owner's
    // name, monospace and packed bytes, the bitmap's width and height (u32),
    // the cell in uv (two floats), and 256 of {u, v, width in cells,
    // advance}, which GH2's revision is drawn by as stored (a grid font's too:
    // UpdateChars only runs for older ones). The offsets are 0 when the body
    // ends short of them.
    struct Font
    {
        size_t cell = 0; // where the cell's two floats are
        float cellWidth = 0.0f;
        float cellHeight = 0.0f;
        Bytes rest;
        std::string chars;   // the characters it has glyphs for
        size_t charsAt = 0;  // their string's length word
        bool packed = false; // CharInfo laid out by hand, not a grid in the characters' order
        size_t bitmapAt = 0; // the bitmap's width and height, then the cell in uv
        size_t infoAt = 0;   // the 256 CharInfo
    };
    std::optional<Font> readFont(const Bytes &body, uint32_t version);

    // The Tex a Mat body (revision 27 on PS2, 28 on 360) has as its diffuse.
    std::optional<std::string> matDiffuse(const Bytes &body, uint32_t version);

    // A Mat body drawn in its Tex's own colours, opaque, with that blend
    // and z mode (Mat's set blend and z_mode). False if it cannot be read.
    bool setMatDrawn(Bytes &body, uint32_t version, uint32_t blend, uint32_t zMode);
}
