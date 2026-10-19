// A hammer-on with the wrong fret leaves that gem to be strummed
// (GuitarTrackWatcherImpl::TryToHopo, retail 0x24dc20), and retail keeps
// the last such gem locked for good: a restart or a practice rewind plays
// it again strum-only. Each jump unlocks it here, as a new song does.

#include "hopo_lock.h"

#include "guest.h"
#include "hook.h"
#include "ps2_runtime_macros.h"

namespace gh2
{
    namespace
    {
        // GuitarTrackWatcherImpl: the gem no longer hammered, -1 from its
        // ctor (0x24d458).
        constexpr uint32_t kHopoLockedGem = 0x8cu;

        struct JumpHookTag;

        // GuitarTrackWatcherImpl::JumpHook, retail 0x24d4e0: clears the
        // pending gem, the HOPO hit and chordFinishedMs, but not the lock.
        // A restart gets here by BeatMatcher::Jump(0) (0x235480).
        void onJumpHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            store<int32_t>(rdram, GPR_U32(ctx, 4) + kHopoLockedGem, -1);
        }
    }

    void installHopoLock(PS2Runtime &runtime, const Addresses &addresses)
    {
        EntryHook<JumpHookTag>::install(runtime, addresses.guitarWatcherJumpHook, onJumpHook);
    }
}
