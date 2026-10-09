// transitions: with snappy_menus, screens change near at once. Retail slides
// one out and the next in over half a second each, taking no input until
// both end.

#include "ui/transitions.h"

#include "guest.h"
#include "hook.h"
#include "ps2_runtime_macros.h"
#include "settings/settings.h"

namespace gh2
{
    namespace
    {
        constexpr float kSlideSeconds = 1.0f / 6.0f;

        // GHScreen: whether it slides at all (animate_transition,
        // CheckAnimateTransition 0x145dc8), and the slide's end and start in
        // UI seconds (AnimateTransition, 0x145f64 and 0x145e98). Draw
        // (0x146160) fades the screen by where the clock is between the two,
        // and once past the end marks the start 1e30, which Exiting waits for
        // (0x1460d8); Entering waits for the end itself (0x14604c).
        constexpr uint32_t kSlides = 0x4cu;
        constexpr uint32_t kSlideEnd = 0x50u;
        constexpr uint32_t kSlideStart = 0x60u;
        constexpr float kNoSlide = 1e30f;

        const Addresses *s_addresses = nullptr;

        struct SlideAnimTag;
        struct EnteringTag;
        struct ExitingTag;

        // RndAnimatable::Animate(from, to, units, period, blend), retail
        // 0x1abbb8: the period in $f14. GHScreen::AnimateTransition (0x145e18)
        // slides the menu camera over 0.5 (0x145f04) as a screen enters and
        // as one leaves, which waits for it too (0x1460bc).
        void onAnimate(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            if (!settings::get(settings::kSnappyMenus))
                return;
            for (const uint32_t slide : s_addresses->menuSlides)
                if (GPR_U32(ctx, 4) == load<uint32_t>(rdram, slide))
                    ctx->f[14] = kSlideSeconds;
        }

        // A sliding screen's end brought in to kSlideSeconds from its start.
        void shortenSlide(uint8_t *rdram, uint32_t screen)
        {
            if (!settings::get(settings::kSnappyMenus) || load<uint32_t>(rdram, screen + kSlides) == 0u)
                return;
            const float start = load<float>(rdram, screen + kSlideStart);
            if (start != kNoSlide && start + kSlideSeconds < load<float>(rdram, screen + kSlideEnd))
                store<float>(rdram, screen + kSlideEnd, start + kSlideSeconds);
        }

        // GHScreen::Entering and Exiting, retail 0x146018 and 0x146088.
        void onEntering(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            shortenSlide(rdram, GPR_U32(ctx, 4));
        }

        void onExiting(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            shortenSlide(rdram, GPR_U32(ctx, 4));
        }
    }

    void installTransitions(PS2Runtime &runtime, const Addresses &addresses)
    {
        s_addresses = &addresses;
        EntryHook<SlideAnimTag>::install(runtime, addresses.rndAnimatableAnimate, onAnimate);
        EntryHook<EnteringTag>::install(runtime, addresses.ghScreenEntering, onEntering);
        EntryHook<ExitingTag>::install(runtime, addresses.ghScreenExiting, onExiting);
    }
}
