// operator>(const Sphere &, const Frustum &), native: whether the sphere is
// wholly behind one of the frustum's six planes. RndDrawable::Draw (0x3d8e08)
// draws nothing for a drawable whose world sphere is.
//
// Retail (0x2d9170) adds the radius to the centre's distance from each plane
// on VU0 and answers with the status register's sticky sign flag (cfc2 vi16,
// 0x2d9254). The recompiled adds set no flags, so the recompiled function
// answers no every time. It is the one function of the game that reads them.
//
// The flags are cleared after the first plane's add (0x2d9214, 0x2d9218).
// That add's flags are taken to land after the clear, as on the hardware,
// so all six planes count.
//
// Only with the frustum_cull setting, which is off: the cull saves the PS2
// drawing and can only take from the picture. The career's ending has it
// regardless: nothing else stops drawing the guitarist the saucer takes.

#include "render/native_cull.h"

#include "guest.h"
#include "hook.h"
#include "settings/settings.h"
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"

#include <cmath>

namespace gh2
{
    namespace
    {
        constexpr uint32_t kRadius = 0x10u;    // the sphere's, after its centre
        constexpr uint32_t kPlanes = 6u;       // each a normal and a distance
        constexpr uint32_t kPlaneStride = 0x10u;
        constexpr uint32_t kStickySign = 0x80u;

        const Addresses *s_addresses = nullptr;
        bool s_ending = false; // from the career's last song won to the game panel's reset or exit

        struct GameOverTag;
        struct ResetTag;
        struct ExitTag;

        void onGameOver(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const R5900Context saved = *ctx;
            const bool won = GPR_U32(ctx, 5) != 0u;
            s_ending = won && runtime->callGuestFunction(rdram, ctx, s_addresses->winCampaignSong,
                                                         {load<uint32_t>(rdram, s_addresses->theGameConfig)}) != 0u;
            *ctx = saved;
        }

        void onGameLeft(uint8_t *, R5900Context *, PS2Runtime *)
        {
            s_ending = false;
        }

        void sphereOutside(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            const uint32_t returnTo = GPR_U32(ctx, 31);
            const uint32_t sphere = GPR_U32(ctx, 4);
            const uint32_t frustum = GPR_U32(ctx, 5);
            const float radius = load<float>(rdram, sphere + kRadius);
            const bool cull = s_ending || settings::get(settings::kFrustumCull);
            bool outside = false;
            for (uint32_t p = 0; cull && p < kPlanes; ++p)
            {
                const uint32_t plane = frustum + p * kPlaneStride;
                float distance = load<float>(rdram, plane) * load<float>(rdram, sphere);
                distance += load<float>(rdram, plane + 4u) * load<float>(rdram, sphere + 4u);
                distance += load<float>(rdram, plane + 8u) * load<float>(rdram, sphere + 8u);
                distance += load<float>(rdram, plane + 12u);
                outside = outside || std::signbit(distance + radius);
            }
            setReturnU32(ctx, outside ? kStickySign : 0u);
            ctx->pc = returnTo;
        }
    }

    void installNativeCull(PS2Runtime &runtime, const Addresses &addresses)
    {
        s_addresses = &addresses;
        runtime.replaceFunction(addresses.sphereOutsideFrustum, sphereOutside);
        EntryHook<GameOverTag>::install(runtime, addresses.gamePanelSetGameOver, onGameOver);
        EntryHook<ResetTag>::install(runtime, addresses.gamePanelReset, onGameLeft);
        EntryHook<ExitTag>::install(runtime, addresses.gamePanelExit, onGameLeft);
    }
}
