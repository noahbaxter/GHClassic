#pragma once

// A PS2 memory card save's 3D icon (.icn, the file icon.sys names): a mesh
// of one or more shapes the browser morphs between, a normal, uv and colour
// a vertex, then a 128x128 picture in 16-bit colour, plain or run-length
// coded. Positions and uvs are 4096ths, y pointing down.

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

namespace gh2::ps2_icon
{
    constexpr int kTextureSide = 128;

    struct Icon
    {
        // shapes[s][v]: vertex v's position in shape s.
        std::vector<std::vector<std::array<float, 3>>> shapes;
        std::vector<std::array<float, 3>> normals;
        std::vector<std::array<float, 2>> uvs;
        std::vector<std::array<uint8_t, 4>> colors; // 0x80 a full one
        // The animation's frames a loop and how fast it plays; one shape or
        // none has nothing to play.
        uint32_t frameLength = 0;
        float speed = 1.0f;
        std::vector<uint8_t> rgba; // kTextureSide square, alpha whole
    };

    // Nullopt for a file that is not one, or whose parts do not add up to
    // its size.
    std::optional<Icon> parse(const std::vector<uint8_t> &file);
}
