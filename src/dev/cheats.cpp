#include "dev/cheats.h"

#include "hook.h"
#include "script.h"

#include <SDL3/SDL_keycode.h>

#include <atomic>
#include <string>

namespace gh2::cheats
{
    namespace
    {
        bool s_enabled = false;
        std::atomic<int> s_win{0}; // stars to win the song with, asked for by a key

        // Run at the next UIManager::Poll, on the game thread.
        struct PollTag;
        void onUiPoll(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const int stars = s_win.exchange(0);
            if (stars == 0)
                return;
            const R5900Context saved = *ctx;
            script::run(rdram, ctx, runtime, "{if {game is_up} {player0 win " + std::to_string(stars) + "}}");
            *ctx = saved;
        }
    }

    void enable()
    {
        s_enabled = true;
    }

    void key(uint32_t keycode)
    {
        if (s_enabled && keycode >= SDLK_F3 && keycode <= SDLK_F5)
            s_win = 3 + static_cast<int>(keycode - SDLK_F3);
    }

    void install(PS2Runtime &runtime, const Addresses &addresses)
    {
        if (s_enabled)
            EntryHook<PollTag>::install(runtime, addresses.uiManagerPoll, onUiPoll);
    }
}
