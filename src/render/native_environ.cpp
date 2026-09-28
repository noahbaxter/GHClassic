// PsEnviron::Select, native: keep the lighting VU1 would have been given.
//
// Retail (0x1a2060) uploads fog, then seven quadwords to qw681..687: the
// ambient colour (+0x40, w 1), three light colours (light +0xc0, w 0) and the
// three lights' directions, transposed. It walks the light list (+0x30) twice.
// A point light (type 0) anywhere wins: its colour, position and range go up
// and program 0x436 lights with it. Otherwise up to three directional lights
// (type 1), each as the negated unit +y axis of its world transform, with
// program 0x6ec. With neither, program 0x7c5 lights by ambient alone. It then
// stores the environ as RndEnviron::sCurrent (RndEnviron::Select, 0x1b6378),
// which PsMat::Select reads for fog, and flushes the packet.
//
// Fog is not taken yet.

#include "render/native_environ.h"

#include "guest.h"
#include "milo/layout.h"
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"

#include <cmath>
#include <iostream>

namespace gh2
{
    namespace
    {
        const Addresses *s_addresses = nullptr;
        Environ s_current;

        void select(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t returnTo = GPR_U32(ctx, 31);
            const uint32_t env = GPR_U32(ctx, 4);

            Environ e;
            for (uint32_t i = 0; i < 3; ++i)
                e.ambient[i] = load<float>(rdram, env + milo::environ::kAmbient + i * 4u);

            bool point = false;
            for (uint32_t node = load<uint32_t>(rdram, env + milo::environ::kFirstLight); node != 0u;
                 node = load<uint32_t>(rdram, node + 4u))
                if (load<uint32_t>(rdram, load<uint32_t>(rdram, node) + milo::light::kType) == milo::light::kPoint)
                    point = true;

            if (point)
            {
                e.kind = Environ::kPoint;
                static bool warned = false;
                if (!warned)
                    std::cerr << "[environ] point light: not implemented, lit by ambient alone" << std::endl;
                warned = true;
            }
            else
            {
                for (uint32_t node = load<uint32_t>(rdram, env + milo::environ::kFirstLight);
                     node != 0u && e.lightCount < 3u; node = load<uint32_t>(rdram, node + 4u))
                {
                    const uint32_t light = load<uint32_t>(rdram, node);
                    if (load<uint32_t>(rdram, light + milo::light::kType) != milo::light::kDirectional)
                        continue;
                    const uint32_t world = static_cast<uint32_t>(
                        runtime->callGuestFunction(rdram, ctx, s_addresses->worldXfm, {light}));
                    float y[3];
                    for (uint32_t c = 0; c < 3; ++c)
                        y[c] = load<float>(rdram, world + 0x10u + c * 4u);
                    // VU0 rsqrt of the squared length, as retail (0x1a2408).
                    const float scale = 1.0f / std::sqrt(y[0] * y[0] + y[1] * y[1] + y[2] * y[2]);
                    for (uint32_t c = 0; c < 3; ++c)
                    {
                        e.toLight[e.lightCount][c] = -(y[c] * scale);
                        e.color[e.lightCount][c] = load<float>(rdram, light + milo::light::kColor + c * 4u);
                    }
                    ++e.lightCount;
                }
                if (e.lightCount != 0u)
                    e.kind = Environ::kDirectional;
            }
            s_current = e;

            store<uint32_t>(rdram, s_addresses->rndEnvironCurrent, env);
            ctx->pc = returnTo;
        }
    }

    const Environ &currentEnviron()
    {
        return s_current;
    }

    void installNativeEnviron(PS2Runtime &runtime, const Addresses &addresses)
    {
        s_addresses = &addresses;
        runtime.replaceFunction(addresses.psEnvironSelect, select);
    }
}
