// Character::DrawShowing (0x161c68) picks one of the character's lods a
// draw: it starts at the lod a camera shot forces (force_char_lod, +0x228,
// below 0 for none) and steps to the next while ComputeScreenSize's answer
// is under the lod's own size, 9% either way of it by which lod it drew last
// (0x161d38). Only lod 0 draws the 360's shadow map (0x161da0).

#include "render/char_lod.h"

#include "guest.h"
#include "hook.h"
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "settings/settings.h"

namespace gh2
{
    namespace
    {
        struct DrawTag;

        constexpr uint32_t kForceLod = 0x228u;

        const Addresses *s_addresses = nullptr;
        PS2Runtime::RecompiledFunction s_computeScreenSize = nullptr;
        // DrawShowing has begun and not yet asked for its size.
        bool s_drawing = false;

        void onDrawShowing(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            if (!settings::get(settings::kHighDetail))
                return;
            store<int32_t>(rdram, GPR_U32(ctx, 4) + kForceLod, -1);
            s_drawing = true;
        }

        // The whole picture's size for DrawShowing, which then stops at lod
        // 0. VideoScreen::Poll (0x28a1e8) asks too, and gets the real one.
        void computeScreenSize(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t returnTo = GPR_U32(ctx, 31);
            runtime->callGuestFunction(rdram, ctx, s_addresses->characterComputeScreenSize,
                                       {GPR_U32(ctx, 4), GPR_U32(ctx, 5)}, s_computeScreenSize);
            if (s_drawing)
                ctx->f[0] = 1.0e9f;
            s_drawing = false;
            ctx->pc = returnTo;
        }
    }

    void installCharLod(PS2Runtime &runtime, const Addresses &addresses)
    {
        s_addresses = &addresses;
        EntryHook<DrawTag>::install(runtime, addresses.characterDrawShowing, onDrawShowing);
        s_computeScreenSize = runtime.lookupFunction(addresses.characterComputeScreenSize);
        runtime.replaceFunction(addresses.characterComputeScreenSize, &computeScreenSize);
    }
}
