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

    // Each filter's weights on the last two samples, in 64ths.
    constexpr int32_t kAdpcmF0[5] = {0, 60, 115, 98, 122};
    constexpr int32_t kAdpcmF1[5] = {0, 0, -52, -55, -60};

    inline void decodeAdpcm(const uint8_t *block, AdpcmState &state, int16_t *out)
    {
        // Shifts past 12 are reserved and act as 9 (psx-spx, "SPU ADPCM").
        const int32_t shift = (block[0] & 0x0f) > 12 ? 9 : (block[0] & 0x0f);
        const int32_t filter = std::min((block[0] >> 4) & 0x0f, 4);
        for (uint32_t i = 0; i < kAdpcmBlockSamples; ++i)
        {
            const uint8_t byte = block[2 + i / 2];
            const int32_t nibble = (i & 1u) ? (byte >> 4) : (byte & 0x0f);
            int32_t s = static_cast<int16_t>(static_cast<uint16_t>(nibble << 12)) >> shift;
            s += (state.hist1 * kAdpcmF0[filter] + state.hist2 * kAdpcmF1[filter] + 32) >> 6;
            s = clamp16(s);
            state.hist2 = state.hist1;
            state.hist1 = s;
            out[i] = static_cast<int16_t>(s);
        }
    }

    // 28 samples as the block that decodes closest to them from `state`,
    // trying every filter at the shift its largest step needs and the one
    // finer; `state` moves on as the decoder's will. Byte 1 is `flags`.
    inline void encodeAdpcm(const int16_t *in, AdpcmState &state, uint8_t flags, uint8_t *block)
    {
        int64_t bestError = INT64_MAX;
        AdpcmState bestState;
        uint8_t best[kAdpcmBlockBytes] = {};
        for (int32_t filter = 0; filter < 5; ++filter)
        {
            // The largest step, predicted from the source as it goes.
            int32_t peak = 0, h1 = state.hist1, h2 = state.hist2;
            for (uint32_t i = 0; i < kAdpcmBlockSamples; ++i)
            {
                const int32_t step = in[i] - ((h1 * kAdpcmF0[filter] + h2 * kAdpcmF1[filter] + 32) >> 6);
                peak = std::max(peak, step < 0 ? -step : step);
                h2 = h1;
                h1 = in[i];
            }
            // A nibble of n stands for n << (12 - shift), and spans -8..7.
            int32_t coarse = 0;
            while (coarse < 12 && (7 << coarse) < peak)
                ++coarse;
            for (int32_t scale = std::max(coarse - 1, 0); scale <= coarse; ++scale)
            {
                const int32_t shift = 12 - scale;
                uint8_t trial[kAdpcmBlockBytes] = {static_cast<uint8_t>(filter << 4 | shift), flags};
                AdpcmState s = state;
                int64_t error = 0;
                for (uint32_t i = 0; i < kAdpcmBlockSamples; ++i)
                {
                    const int32_t predicted = (s.hist1 * kAdpcmF0[filter] + s.hist2 * kAdpcmF1[filter] + 32) >> 6;
                    const int32_t step = in[i] - predicted;
                    const int32_t half = scale > 0 ? 1 << (scale - 1) : 0;
                    int32_t nibble = (step >= 0 ? step + half : step - half) / (1 << scale);
                    nibble = std::clamp(nibble, -8, 7);
                    const int32_t out = clamp16((nibble << scale) + predicted);
                    s.hist2 = s.hist1;
                    s.hist1 = out;
                    error += int64_t(out - in[i]) * (out - in[i]);
                    trial[2 + i / 2] |= static_cast<uint8_t>((nibble & 0x0f) << ((i & 1u) ? 4 : 0));
                }
                if (error < bestError)
                {
                    bestError = error;
                    bestState = s;
                    std::copy(trial, trial + kAdpcmBlockBytes, best);
                }
            }
        }
        state = bestState;
        std::copy(best, best + kAdpcmBlockBytes, block);
    }
}
