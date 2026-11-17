#pragma once

// Each seat's colour, P1 to P4, from each game's own splash and main menu
// art: its ring (ui/help_bar.cpp) and its Start menu's poster
// (ui/players.cpp). GH2's red, gold and flame orange; GH1's flame orange,
// sunburst yellow and guitar red; GH80s's pink, grid green and blue.

#include <array>

namespace gh2::seat_colors
{
    struct Game
    {
        const char *game;
        std::array<std::array<float, 3>, 4> seats;
    };
    inline constexpr std::array<Game, 3> kGames = {{
        {"gh2", {{{0.8f, 0.18f, 0.14f}, {0.86f, 0.66f, 0.22f}, {0.9f, 0.45f, 0.12f}, {0.89f, 0.84f, 0.7f}}}},
        {"gh1", {{{0.92f, 0.47f, 0.12f}, {0.93f, 0.78f, 0.2f}, {0.75f, 0.14f, 0.15f}, {0.89f, 0.84f, 0.7f}}}},
        {"gh80s", {{{0.86f, 0.16f, 0.55f}, {0.62f, 0.82f, 0.22f}, {0.22f, 0.62f, 0.86f}, {0.55f, 0.27f, 0.78f}}}},
    }};
}
