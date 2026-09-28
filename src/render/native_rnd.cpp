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
        // Aspect index (+0xd4), which Rnd::YRatio (0x1d4c20) looks up in
        // {1, 0.75, 0.5625}.
        constexpr uint32_t kAspect = 0xd4u;
        constexpr float kYRatios[] = {1.0f, 0.75f, 0.5625f};
        constexpr uint32_t kAspectWidescreen = 2u; // what Options::SetWideScreen(true) sets

        struct BeginTag;
        struct EndTag;

        void onBeginDrawing(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            const uint32_t rnd = GPR_U32(ctx, 4);
            Frame &frame = building();
            const uint64_t serial = frame.serial + 1u;
            frame = Frame{};
            frame.serial = serial;
            for (uint32_t i = 0; i < 4; ++i)
                frame.clear[i] = load<float>(rdram, rnd + kClearColor + i * 4u);
            const int32_t width = load<int32_t>(rdram, rnd + kWidth);
            const int32_t height = load<int32_t>(rdram, rnd + kHeight);
            if (width > 0 && height > 0)
            {
                frame.width = static_cast<uint32_t>(width);
                frame.height = static_cast<uint32_t>(height);
            }
            const uint32_t aspect = load<uint32_t>(rdram, rnd + kAspect);
            if (aspect < 3u)
                frame.yRatio = kYRatios[aspect];
            frame.displayAspect = aspect == kAspectWidescreen ? 16.0f / 9.0f : 4.0f / 3.0f;
        }

        // Every draw of the frame has been recorded by the time EndDrawing
        // starts.
        void onEndDrawing(uint8_t *, R5900Context *, PS2Runtime *)
        {
            frames().publish(building());
        }
    }

    void installNativeRnd(PS2Runtime &runtime, const Addresses &addresses)
    {
        EntryHook<BeginTag>::install(runtime, addresses.psRndBeginDrawing, onBeginDrawing);
        EntryHook<EndTag>::install(runtime, addresses.psRndEndDrawing, onEndDrawing);
    }
}
