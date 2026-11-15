#pragma once

#include "addresses.h"

#include <cstdint>

class PS2Runtime;
struct R5900Context;

namespace gh2
{
    void installLatency(PS2Runtime &runtime, const Addresses &addresses);
    // The hit window's offset given to the song in play again: it is in song
    // time, so it follows the speed (content/song_speed.h).
    void pushSyncOffset(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
}
