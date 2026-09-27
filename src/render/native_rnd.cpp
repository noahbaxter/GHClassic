// Frame boundaries for the native renderer. The guest's own PsRnd begin and
// end still run (pacing, bookkeeping, the null FlushPacket); these hooks only
// read what a frame needs on entry and hand it to the host.

#include "render/native_rnd.h"

#include "guest.h"
#include "hook.h"
#include "ps2_runtime_macros.h"
#include "render/frame.h"

namespace gh2
{
    namespace
    {
        // Rnd fields: clear colour (Hmx::Color, 4 floats) at +0x30, which
        // PsRnd::SwapBuffers clears with (0x19ad44); width and height at
        // +0x40/+0x44 (0x19ad84, 0x19ad7c).
        constexpr uint32_t kClearColor = 0x30u;
        constexpr uint32_t kWidth = 0x40u;
        constexpr uint32_t kHeight = 0x44u;

        Frame s_building;

        struct BeginTag;
        struct EndTag;

        void onBeginDrawing(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            const uint32_t rnd = GPR_U32(ctx, 4);
            s_building = Frame{};
            s_building.serial = frames().latest().serial + 1u;
            for (uint32_t i = 0; i < 4; ++i)
                s_building.clear[i] = load<float>(rdram, rnd + kClearColor + i * 4u);
            const int32_t width = load<int32_t>(rdram, rnd + kWidth);
            const int32_t height = load<int32_t>(rdram, rnd + kHeight);
            if (width > 0 && height > 0)
            {
                s_building.width = static_cast<uint32_t>(width);
                s_building.height = static_cast<uint32_t>(height);
            }
        }

        // Every draw of the frame has been recorded by the time EndDrawing
        // starts.
        void onEndDrawing(uint8_t *, R5900Context *, PS2Runtime *)
        {
            frames().publish(s_building);
        }
    }

    void installNativeRnd(PS2Runtime &runtime, const Addresses &addresses)
    {
        EntryHook<BeginTag>::install(runtime, addresses.psRndBeginDrawing, onBeginDrawing);
        EntryHook<EndTag>::install(runtime, addresses.psRndEndDrawing, onEndDrawing);
    }
}
