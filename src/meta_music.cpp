// Menu music fades carried across screen changes.
//
// MetaMusic::Poll (0x21f180) fades its stream in over one second on a
// LinearInterpolator at +0x80, keyed on TaskMgr::UISeconds * 1000: Reset
// (-96 dB, target, now, now + 1000 / rate) at 0x21f378, then each poll the
// volume is slope * now + intercept, clamped to [-96 dB, target]. Every
// screen change zeroes UISeconds. On a PS2 the fade starts after the first
// one: ReadyToPlay (0x21f138) waits for 200 KB of the stream, which the drive
// takes longer to deliver than the switch to bootup_load. Disc reads here are
// instant, the fade starts first, and after the reset its window lies
// seconds ahead, so the boot music sits at -96 dB until it is replaced.
//
// So when the UI clock goes back between polls, the fade's window goes back
// with it and the fade carries on from where it was.

#include "meta_music.h"

#include "guest.h"
#include "hook.h"
#include "ps2_runtime_macros.h"

#include <map>

namespace gh2
{
    namespace
    {
        // LinearInterpolator, from Reset (0x2d96b8).
        constexpr uint32_t kFade = 0x80u;
        constexpr uint32_t kX0 = 0x08u, kX1 = 0x0cu, kSlope = 0x14u, kIntercept = 0x18u;

        const Addresses *s_addresses = nullptr;
        std::map<uint32_t, float> s_lastUi; // by MetaMusic

        // TaskMgr::UISeconds (0x2c6750).
        float uiSeconds(uint8_t *rdram)
        {
            return load<float>(rdram, load<uint32_t>(rdram, s_addresses->theTaskMgr + 0x28u) + 0x34u);
        }

        void add(uint8_t *rdram, uint32_t address, float delta)
        {
            store<float>(rdram, address, load<float>(rdram, address) + delta);
        }

        struct PollTag;

        void onPoll(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            const uint32_t self = GPR_U32(ctx, 4);
            const float now = uiSeconds(rdram);
            auto [it, first] = s_lastUi.try_emplace(self, now);
            const float back = it->second - now;
            it->second = now;
            if (first || back <= 0.0f)
                return;
            const uint32_t fade = self + kFade;
            const float ms = back * 1000.0f;
            add(rdram, fade + kX0, -ms);
            add(rdram, fade + kX1, -ms);
            add(rdram, fade + kIntercept, load<float>(rdram, fade + kSlope) * ms);
        }
    }

    void installMetaMusic(PS2Runtime &runtime, const Addresses &addresses)
    {
        s_addresses = &addresses;
        EntryHook<PollTag>::install(runtime, addresses.metaMusicPoll, onPoll);
    }
}
