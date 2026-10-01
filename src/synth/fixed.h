#pragma once

// The SPU2's integer sample arithmetic.

#include <algorithm>
#include <cstdint>

namespace gh2::synth
{
    // Saturated to a 16-bit sample.
    inline int32_t clamp16(int32_t v)
    {
        return std::clamp(v, -32768, 32767);
    }

    // a scaled by b, a Q15 fraction (0x8000 is 1.0).
    inline int32_t mul15(int32_t a, int32_t b)
    {
        return (a * b) >> 15;
    }

    // x into 0..n-1, for x of either sign.
    inline int32_t wrap(int32_t x, int32_t n)
    {
        return ((x % n) + n) % n;
    }
}
