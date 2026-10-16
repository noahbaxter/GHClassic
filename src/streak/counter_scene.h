#pragma once

// The streak counter's scene (counter.cpp): its own, made from the score's,
// and a copy of the score display in each HUD scene that draws it.

#include "disc/ark.h"
#include "milo/milo.h"

#include <cstddef>
#include <optional>
#include <string>

namespace gh2::streak_counter
{
    // Not streak_display: that name is the disc's own, the multiplier's.
    inline constexpr const char *kScenePath = "hud/gen/streak_counter.milo_ps2";

    // Its wheels, the icon's first and the ones last. Each has its own copy
    // of the score's digit Mats, streak_w<wheel>_<digit>.mat, so one turning
    // fades alone.
    inline constexpr size_t kWheels = 4u;
    std::string wheelMat(size_t wheel, int digit);

    // score_display.milo as the counter, at kScenePath.
    std::optional<ark::Made> makeScene();

    // A copy of the score display that draws the counter's scene, in the
    // score meter's Group after the score's. False when the scene has no
    // score display there.
    bool addCounter(milo::Dir &scene);
}
