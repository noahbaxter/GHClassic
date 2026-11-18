#pragma once

// The game's loaders' hottest reads, made in one go. Its loaders read a
// value at a time, each through BinStream::ReadEndian (0x2c8e20), Read
// (0x2c8c50) and the stream's own ReadImpl: a character's clips, sample by
// sample, cost the game thread more than anything else a screen loads, and a
// mesh's verts next. Here a sample, or a vert, is read with one Read and laid
// out and byte swapped natively, into the same memory the game's own leaves.

#include "addresses.h"

class PS2Runtime;

namespace gh2::bin_reads
{
    void install(PS2Runtime &runtime, const Addresses &addresses);
}
