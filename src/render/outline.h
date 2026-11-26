#pragma once

// outline: a border round the ink of a texture no font draws with, in any
// colour, past its edge. mesh.frag draws it from the texture's alpha. Its
// width is a share of the texture's height, so it looks the same at every
// resolution and size. It draws only inside the texture's quad: ink nearer
// the edge than the width has its border cut square there.

#include <cstdint>
#include <optional>

namespace gh2::outline
{
    struct Border
    {
        uint32_t rgba8 = 0; // r in the low byte, as mesh.frag unpacks it
        float width = 0.0f; // a share of the texture's height
    };

    // Game thread only. A texture's border goes with it (clear as it is
    // destroyed, forgetAll as the game's textures are let go).
    void set(uint32_t tex, const Border &border);
    void clear(uint32_t tex);
    void forgetAll();
    std::optional<Border> of(uint32_t tex);
}
