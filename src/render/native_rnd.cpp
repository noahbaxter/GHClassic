// Frame boundaries for the native renderer. The guest's own PsRnd begin and
// end still run (pacing, bookkeeping, the null FlushPacket); these hooks only
// read what a frame needs on entry and hand it to the host.

#include "render/native_rnd.h"

#include "guest.h"
#include "hook.h"
#include "milo/layout.h"
#include "ps2_runtime_macros.h"
#include "render/frame.h"

#include <algorithm>
#include <iterator>

namespace gh2
{
    namespace
    {
        constexpr float kYRatios[] = {1.0f, 0.75f, 0.5625f}; // Rnd::YRatio, by aspect index
        constexpr uint32_t kAspectWidescreen = 2u; // what Options::SetWideScreen(true) sets

        struct BeginTag;
        struct EndTag;

        void onBeginDrawing(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            const auto rnd = load<milo::Rnd>(rdram, GPR_U32(ctx, 4));
            Frame &frame = building();
            const uint64_t serial = frame.serial + 1u;
            frame = Frame{};
            frame.serial = serial;
            std::copy(std::begin(rnd.clearColor.rgba), std::end(rnd.clearColor.rgba), frame.clear);
            if (rnd.width > 0 && rnd.height > 0)
            {
                frame.width = static_cast<uint32_t>(rnd.width);
                frame.height = static_cast<uint32_t>(rnd.height);
            }
            if (rnd.aspect < 3u)
                frame.yRatio = kYRatios[rnd.aspect];
            frame.displayAspect = rnd.aspect == kAspectWidescreen ? 16.0f / 9.0f : 4.0f / 3.0f;
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
