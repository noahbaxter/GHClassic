#pragma once

// PS-ADPCM, the SPU2's sample format: 16-byte blocks of a shift and filter
// byte, a flag byte, then 28 four-bit samples, low nibble first.

#include "synth/fixed.h"

#include <algorithm>
#include <cstdint>

namespace gh2::synth
{
    constexpr uint32_t kAdpcmBlockBytes = 16u;
    constexpr uint32_t kAdpcmBlockSamples = 28u;

    // Byte 1 of a block, read by an SPU2 voice walking sound RAM.
    constexpr uint8_t kAdpcmLoopEnd = 0x01u;
    constexpr uint8_t kAdpcmRepeat = 0x02u;
    constexpr uint8_t kAdpcmLoopStart = 0x04u;

    // Samples and bytes in whole blocks, signed so offsets can run backwards.
    constexpr int32_t adpcmBytes(int32_t samples)
    {
        return samples / static_cast<int32_t>(kAdpcmBlockSamples) * static_cast<int32_t>(kAdpcmBlockBytes);
    }
    constexpr int32_t adpcmSamples(int32_t bytes)
    {
        return bytes * static_cast<int32_t>(kAdpcmBlockSamples) / static_cast<int32_t>(kAdpcmBlockBytes);
    }

    struct AdpcmState
    {
        int32_t hist1 = 0;
        int32_t hist2 = 0;
    };

    inline void decodeAdpcm(const uint8_t *block, AdpcmState &state, int16_t *out)
    {
        static constexpr int32_t kF0[5] = {0, 60, 115, 98, 122};
        static constexpr int32_t kF1[5] = {0, 0, -52, -55, -60};
        // Shifts past 12 are reserved and act as 9 (psx-spx, "SPU ADPCM").
        const int32_t shift = (block[0] & 0x0f) > 12 ? 9 : (block[0] & 0x0f);
        const int32_t filter = std::min((block[0] >> 4) & 0x0f, 4);
        for (uint32_t i = 0; i < kAdpcmBlockSamples; ++i)
        {
            const uint8_t byte = block[2 + i / 2];
            const int32_t nibble = (i & 1u) ? (byte >> 4) : (byte & 0x0f);
            int32_t s = static_cast<int16_t>(static_cast<uint16_t>(nibble << 12)) >> shift;
            s += (state.hist1 * kF0[filter] + state.hist2 * kF1[filter] + 32) >> 6;
            s = clamp16(s);
            state.hist2 = state.hist1;
            state.hist1 = s;
            out[i] = static_cast<int16_t>(s);
        }
    }
}
