#include "streak/counter_motion.h"

#include "guest.h"
#include "milo/layout.h"
#include "ps2_runtime.h"
#include "runtime/host_clock.h"

#include <algorithm>

namespace gh2::streak_counter
{
    namespace
    {
        // Seconds for the whole way: out unhurried, back in a little quicker.
        constexpr float kOut = 0.5f, kIn = 0.45f;
        // A turn's seconds, and how far below its place, in the display's
        // space, a new digit starts: as GH3's, and still inside its wheel.
        constexpr float kTurn = 0.1f, kRise = 5.0f;

        // Its Mat's colour's alpha, which the draw reads (native_mat.cpp).
        void fade(uint8_t *rdram, uint32_t mat, float alpha)
        {
            store<float>(rdram, mat + ::milo::mat::kColor + 12u, alpha);
        }

        // A wheel a frame into its turn: its Mat's alpha and its mesh's
        // height, linear in and rising to rest slowing.
        void turn(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, const Addresses &addresses,
                  Counter::Wheel &wheel, float dt)
        {
            if (!wheel.turning || !wheel.mat || !wheel.mesh)
                return;
            const float t = wheel.along = std::min(1.0f, wheel.along + dt / kTurn), left = 1.0f - t;
            fade(rdram, wheel.mat, t);
            const uint32_t transform = wheel.mesh + ::milo::mesh::kTransform;
            const uint32_t z = transform + kLocalPos + 8u;
            if (std::isnan(wheel.rest))
                wheel.rest = load<float>(rdram, z);
            store<float>(rdram, z, wheel.rest - kRise * left * left);
            runtime->callGuestFunction(rdram, ctx, addresses.setDirty, {transform});
            wheel.turning = t < 1.0f;
        }
    }

    void turnTo(uint8_t *rdram, Counter::Wheel &wheel, uint32_t mat, uint32_t mesh, bool atOnce)
    {
        wheel.mat = mat;
        wheel.mesh = mesh;
        // There at once is a turn already done, which the next frame
        // settles: back to rest, should one have been under way.
        wheel.along = atOnce ? 1.0f : 0.0f;
        wheel.turning = true;
        fade(rdram, mat, wheel.along);
    }

    void step(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, const Addresses &addresses, uint32_t display,
              Counter &counter)
    {
        const auto now = ps2x::host_clock::now();
        const float dt = counter.last == std::chrono::steady_clock::time_point{}
                             ? 0.0f
                             : std::min(0.1f, std::chrono::duration<float>(now - counter.last).count());
        counter.last = now;
        counter.out = counter.shown ? std::min(1.0f, counter.out + dt / kOut) : std::max(0.0f, counter.out - dt / kIn);
        if (counter.shown)
            counter.showing = counter.streak;
        for (Counter::Wheel &wheel : counter.wheels)
            turn(rdram, ctx, runtime, addresses, wheel, dt);
        if (!counter.placed)
            return;
        for (Counter::Raised &raised : counter.raised)
        {
            const uint32_t z = raised.view + kLocalPos + 8u;
            const float there = load<float>(rdram, z);
            if (there == raised.written)
                continue;
            raised.written = there + counter.lift;
            store<float>(rdram, z, raised.written);
            runtime->callGuestFunction(rdram, ctx, addresses.setDirty, {raised.view});
        }
        const float in = 1.0f - counter.out, eased = 1.0f - in * in * in;
        const float place[3] = {counter.at[0], counter.at[1], counter.at[2] - counter.drop * (1.0f - eased)};
        const uint32_t pos = display + kDisplayTrans + kLocalPos;
        if (load<float>(rdram, pos) == place[0] && load<float>(rdram, pos + 4u) == place[1] &&
            load<float>(rdram, pos + 8u) == place[2])
            return;
        for (uint32_t axis = 0; axis < 3u; ++axis)
            store<float>(rdram, pos + 4u * axis, place[axis]);
        runtime->callGuestFunction(rdram, ctx, addresses.setDirty, {display + kDisplayTrans});
    }
}
