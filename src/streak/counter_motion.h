#pragma once

// The streak counter's moves, a frame at a time (counter.cpp): its slide out
// from behind the score shell and back in, and the meters it raises.

#include "addresses.h"

#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <vector>

class PS2Runtime;
struct R5900Context;

namespace gh2::streak_counter
{
    // Its RndTransformable, its local xfm's place at +0x50 there
    // (OnSetLocalPos, 0x1df038).
    constexpr uint32_t kDisplayTrans = 0xe0u, kLocalPos = 0x50u;

    // A counter, by its display's address: the streak it shows in place of
    // the score the HUD gives it each frame, the Mats of its ones wheel by
    // digit, and its icon's.
    struct Counter
    {
        int32_t streak = 0;
        std::array<uint32_t, 10> ones{};
        uint32_t icon = 0;
        // Its slide: where it rests, how far below that it hides, `out` how
        // far along it is (0 in, 1 out), and the streak it shows, the last
        // it was out with while it goes back in.
        bool shown = false, placed = false;
        float at[3] = {}, drop = 0.0f, out = 0.0f;
        int32_t showing = 0;
        std::chrono::steady_clock::time_point last{};
        // The views it raises all song (the score meter's, which it hangs
        // under, and the rock meter's, to match), by `lift`, over wherever
        // their own anims put them: what was last written there.
        struct Raised
        {
            uint32_t view = 0;
            float written = NAN;
        };
        std::vector<Raised> raised;
        float lift = 0.0f;
    };

    // A frame of the counter: its views raised over where their anims (the
    // slide in, the shake) last left them, left alone when as last written;
    // and its slide a step on, eased out on the way out, so it goes back in
    // gathering speed.
    void step(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, const Addresses &addresses, uint32_t display,
              Counter &counter);
}
