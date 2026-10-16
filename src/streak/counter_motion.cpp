#include "streak/counter_motion.h"

#include "guest.h"
#include "ps2_runtime.h"

#include <algorithm>

namespace gh2::streak_counter
{
    namespace
    {
        // Seconds for the whole way: out unhurried, back in a little quicker.
        constexpr float kOut = 0.5f, kIn = 0.45f;
    }

    void step(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, const Addresses &addresses, uint32_t display,
              Counter &counter)
    {
        const auto now = std::chrono::steady_clock::now();
        const float dt = counter.last == std::chrono::steady_clock::time_point{}
                             ? 0.0f
                             : std::min(0.1f, std::chrono::duration<float>(now - counter.last).count());
        counter.last = now;
        counter.out = counter.shown ? std::min(1.0f, counter.out + dt / kOut) : std::max(0.0f, counter.out - dt / kIn);
        if (counter.shown)
            counter.showing = counter.streak;
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
