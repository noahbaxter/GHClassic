// star_power_overlap: a star phrase completed during star power is
// captured as one outside it is, under either note engine. Retail's StarPower::Hit (0x11e2b8) reads
// mUsing (+0x2c) at 0x11e30c and, while using, treats a phrase's gem as the
// phrase missed (EnterMissedState, 0x11e7c8). With mUsing read as off for
// the call, the phrase's gems are tracked, its last gem adds the phrase's
// worth (AddValue, 0x11eee8: the bar, at most full, that star power drains
// from, so it lasts longer) and phrase_captured plays its sound; star power
// stays on, as the flag is put back. Off, none of this is installed. Its
// gems look a phrase's under star power (highway/cyan_stars.h).

#include "gameplay/engine/star_power_overlap.h"

#include "guest/star_power.h"
#include "hook.h"
#include "settings/settings.h"

#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"

namespace gh2
{
    namespace
    {
        const Addresses *s_addresses = nullptr;

        PS2Runtime::RecompiledFunction s_hit = nullptr;

        // StarPower::Hit(int gem).
        void hit(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const guest::StarPower star(rdram, GPR_U32(ctx, 4));
            const bool active = star.active();
            star.setActive(false);
            callOriginal(rdram, ctx, runtime, s_addresses->starPowerHit, s_hit);
            star.setActive(active);
        }
    }

    void installStarPowerOverlap(PS2Runtime &runtime, const Addresses &addresses)
    {
        if (!settings::get(settings::kStarPowerOverlap))
            return;
        s_addresses = &addresses;
        replaceKeeping(runtime, addresses.starPowerHit, &hit, s_hit);
    }
}
