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

    // The tables made since the first `known`, appended to `out`. Any thread.
    void tablesSince(size_t known, std::vector<std::vector<uint32_t>> &out);
}
