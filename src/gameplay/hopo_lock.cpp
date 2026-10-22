// A hammer-on with the wrong fret leaves that gem to be strummed
// (GuitarTrackWatcherImpl::TryToHopo, retail 0x24dc20), and retail keeps
// the last such gem locked for good: a restart or a practice rewind plays
// it again strum-only. Each jump unlocks it here, as a new song does.

#include "gameplay/hopo_lock.h"

#include "guest/beatmatch.h"
#include "hook.h"
#include "ps2_runtime_macros.h"

namespace gh2
{
    namespace
    {
        struct JumpHookTag;

        // GuitarTrackWatcherImpl::JumpHook, retail 0x24d4e0: clears the
        // pending gem, the HOPO hit and chordFinishedMs, but not the lock,
        // -1 from its ctor (0x24d458). A restart gets here by
        // BeatMatcher::Jump(0) (0x235480).
        void onJumpHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            guest::GuitarTrackWatcherImpl(rdram, GPR_U32(ctx, 4)).setHopoLockedGem(-1);
        }
    }

    void installHopoLock(PS2Runtime &runtime, const Addresses &addresses)
    {
        EntryHook<JumpHookTag>::install(runtime, addresses.guitarWatcherJumpHook, onJumpHook);
    }
}
