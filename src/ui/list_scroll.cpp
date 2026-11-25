#include "ui/list_scroll.h"

#include "guest.h"
#include "hook.h"
#include "runtime/host_clock.h"
#include "script.h"

#include <algorithm>
#include <chrono>
#include <unordered_map>

namespace gh2::list_scroll
{
    namespace
    {
        using Clock = std::chrono::steady_clock;

        constexpr uint32_t kPadDUp = 12u, kPadDDown = 14u; // JoypadButton
        // A screen's ask lasts this long, a few frames at the slowest.
        constexpr auto kAskLasts = std::chrono::milliseconds(150);
        // How long up or down is held before the list speeds up.
        constexpr float kFastAfterSeconds = 2.0f;
        // Then a row every this many frames (at 60 fps): 2 is 30 rows a
        // second, 3 is 20, 4 is 15. The game's own pace is about 7 to 10.
        constexpr int kFastFramesPerRow = 3;

        constexpr auto kFastAfter = std::chrono::milliseconds(static_cast<int>(kFastAfterSeconds * 1000.0f));
        // A repeat goes once its time has passed, checked once a frame: half
        // a frame short of the frames wanted lands on that frame.
        constexpr float kFastMs = (kFastFramesPerRow - 0.5f) * 1000.0f / 60.0f;

        Clock::time_point s_asked;
        // Up or down held, by JoypadRepeat: since when.
        std::unordered_map<uint32_t, Clock::time_point> s_held;

        bool asked(Clock::time_point now) { return now - s_asked <= kAskLasts; }

        struct StartTag;
        void onStart(uint8_t *, R5900Context *ctx, PS2Runtime *)
        {
            const uint32_t button = GPR_U32(ctx, 5);
            if (button == kPadDUp || button == kPadDDown)
                s_held[GPR_U32(ctx, 4)] = ps2x::host_clock::now();
            else
                s_held.erase(GPR_U32(ctx, 4));
        }

        struct ResetTag;
        void onReset(uint8_t *, R5900Context *ctx, PS2Runtime *) { s_held.erase(GPR_U32(ctx, 4)); }

        struct PollTag;
        void onPoll(uint8_t *, R5900Context *ctx, PS2Runtime *)
        {
            const auto now = ps2x::host_clock::now();
            const auto held = s_held.find(GPR_U32(ctx, 4));
            if (asked(now) && held != s_held.end() && now - held->second >= kFastAfter)
                ctx->f[13] = std::min(ctx->f[13], kFastMs);
        }

        // {list_scroll_fast}: this frame's screen scrolls fast. TRUE while
        // up or down has been held there long enough to speed up, for what
        // a row's move shows to wait until it slows.
        script::Node fast(const script::Call &)
        {
            const auto now = ps2x::host_clock::now();
            const bool rushing = asked(now) && std::any_of(s_held.begin(), s_held.end(), [&](const auto &h)
                                                           { return now - h.second >= kFastAfter; });
            s_asked = now;
            return {rushing ? 1u : 0u, script::kInt};
        }
    }

    void install(PS2Runtime &runtime, const Addresses &addresses)
    {
        script::addCommand("list_scroll_fast", fast);
        EntryHook<StartTag>::install(runtime, addresses.joypadRepeatStart, onStart);
        EntryHook<ResetTag>::install(runtime, addresses.joypadRepeatReset, onReset);
        EntryHook<PollTag>::install(runtime, addresses.joypadRepeatPoll, onPoll);
    }
}
