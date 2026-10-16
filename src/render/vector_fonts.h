#pragma once

// Text drawn from the traced fonts' outlines (fonts/, tools/fontpack.py) in
// place of their texture's pixels. A RndText's glyphs are quads into its
// font's texture, laid out by the game as ever; mesh.frag finds the glyph a
// pixel's uv falls in and tests the pixel against that glyph's outline.
//
// The buffer mesh.frag reads holds the pack's glyphs once, and after them a
// table for each font a text has drawn with: the uv rectangle each of its
// characters' quads covers, where that character's cell starts, and the
// glyph it is in the pack.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace gh2::vector_fonts
{
    // A text draws with `font` through texture `tex`: the table its quads
    // are drawn from, made the first time, if the pack has that font. Game
    // thread only.
    std::optional<uint32_t> noteText(uint8_t *rdram, uint32_t font, uint32_t tex);

    // The table a texture's text draws from, if any. Game thread only.
    std::optional<uint32_t> tableOf(uint32_t tex);

    // The texture is gone; or every one is.
    void forget(uint32_t tex);
    void forgetAll();

    // The pack's glyphs, as mesh.frag's buffer starts. Any thread.
    const std::vector<uint32_t> &glyphs();

    // The pack's fonts' names. Any thread.
    std::vector<std::string> names();

    // The pack's glyph for `code` in font `name` (with or without ".font"):
    // its first layer, the font's cell in the pack's pixels, which a
    // character's cell in its sheet spans, and the layer's bounds in them
    // (x0, y0, x1, y1). None if the pack has no such glyph. Any thread.
    struct Glyph
    {
        uint32_t layer = 0;
        float cell[2] = {};
        float bounds[4] = {};
    };
    std::optional<Glyph> glyph(std::string name, uint8_t code);

    // Point (x, y), in the pack's pixels, is inside glyph layer `layer`, as
    // mesh.frag tests it. Any thread.
    bool inside(uint32_t layer, float x, float y);

    // Glyph layer `layer`'s coverage, 0 to 1, of each pixel of a sheet from
    // (x0, y0) to (x1, y1), rows in turn, with its glyph's cell at sheet
    // pixel (ox, oy) and a sheet pixel spanning (sx, sy) of the pack's: a
    // sheet's own pixels as mesh.frag would cover them. Any thread.
    std::vector<float> coverage(uint32_t layer, int x0, int y0, int x1, int y1, float ox, float oy, float sx, float sy);

    // The tables made since the first `known`, appended to `out`. Any thread.
    void tablesSince(size_t known, std::vector<std::vector<uint32_t>> &out);
}
