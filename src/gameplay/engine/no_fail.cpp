#include "gameplay/engine/no_fail.h"

#include "dev/timing.h"
#include "guest.h"
#include "hook.h"
#include "ps2_runtime.h"
#include "settings/settings.h"

#include <cstdio>
#include <cstring>
#include <iostream>

namespace gh2
{
    namespace
    {
        const Addresses *s_addresses = nullptr;
        bool s_failed = false;

        bool quickplay(uint8_t *rdram)
        {
            const uint32_t config = load<uint32_t>(rdram, s_addresses->theGameConfig);
            const uint32_t mode = config ? load<uint32_t>(rdram, config + 0x3cu) : 0u;
            return mode != 0u && std::strcmp(reinterpret_cast<const char *>(getMemPtr(rdram, mode)), "quickplay") == 0;
        }

        // Performer::LoseGame(this), retail 0x10fe30: GamePanel::SetGameOver(false).
        struct LoseTag;
        bool onLose(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            if (!settings::get(settings::kNoFail) || !quickplay(rdram))
                return false;
            if (!s_failed)
            {
                char line[80];
                std::snprintf(line, sizeof line, "[song] failed at %.1f s, played on (no_fail)", timing::songSeconds());
                std::cerr << line << std::endl;
            }
            s_failed = true;
            leave(ctx);
            return true;
        }

        // TrackPanel::Reset, as a song starts and on each restart.
        struct ResetTag;
        void onReset(uint8_t *, R5900Context *, PS2Runtime *)
        {
            s_failed = false;
        }
    }

    void installNoFail(PS2Runtime &runtime, const Addresses &addresses)
    {
        s_addresses = &addresses;
        Override<LoseTag>::install(runtime, addresses.performerLoseGame, onLose);
        EntryHook<ResetTag>::install(runtime, addresses.trackPanelReset, onReset);
    }

    bool failedThisPlay()
    {
        return s_failed;
    }
}
