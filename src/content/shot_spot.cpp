#include "content/shot_spot.h"

#include "content/campaigns.h"
#include "guest.h"
#include "hook.h"
#include "milo/layout.h"

namespace gh2::shot_spot
{
    namespace
    {
        const Addresses *s_addresses = nullptr;
        PS2Runtime::RecompiledFunction s_interp = nullptr;

        // (CamShotFrame *from, CamShot *, CamShotFrame *to, RndCam *, ...).
        void interp(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t cam = GPR_U32(ctx, 7);
            const float yScale = -load<float>(rdram, cam + milo::camera::kYScale);
            const float w = load<float>(rdram, cam + milo::camera::kRect + 8u);
            const float h = load<float>(rdram, cam + milo::camera::kRect + 12u);
            if (campaigns::venueGame() != "gh1" || yScale <= 0.0f || w <= 0.0f || h <= 0.0f)
            {
                s_interp(rdram, ctx, runtime);
                return;
            }
            const float wide = load<float>(rdram, cam + milo::camera::kXScale) / yScale / (0.75f * h / w);
            const uint32_t keys[2] = {GPR_U32(ctx, 4) + milo::camShotFrame::kScreenSpot,
                                      GPR_U32(ctx, 6) + milo::camShotFrame::kScreenSpot};
            const float was[2] = {load<float>(rdram, keys[0]), load<float>(rdram, keys[1])};
            store<float>(rdram, keys[0], was[0] * wide);
            if (keys[1] != keys[0])
                store<float>(rdram, keys[1], was[1] * wide);
            callOriginal(rdram, ctx, runtime, s_addresses->camShotFrameInterp, s_interp);
            store<float>(rdram, keys[0], was[0]);
            store<float>(rdram, keys[1], was[1]);
        }
    }

    void install(PS2Runtime &runtime, const Addresses &addresses)
    {
        s_addresses = &addresses;
        replaceKeeping(runtime, addresses.camShotFrameInterp, interp, s_interp);
    }
}
