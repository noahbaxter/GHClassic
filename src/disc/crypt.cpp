#include "disc/crypt.h"

#include <array>
#include <cstring>

namespace gh2::crypt
{
    namespace
    {
        uint32_t seedOf(const Bytes &file)
        {
            uint32_t seed = 0u;
            if (file.size() >= 4u)
                std::memcpy(&seed, file.data(), 4u);
            return seed;
        }
    }

    // BinStream::EnableReadEncryption (GH1 0x2407e8) seeds Rand (0x265940)
    // with the first word; Read (0x2408d0) XORs each byte with Rand::Int
    // (0x265aa0).
    Bytes randStream(const Bytes &file)
    {
        uint32_t seed = seedOf(file);
        std::array<uint32_t, 256> table;
        for (uint32_t &t : table)
        {
            const uint32_t a = seed * 0x41c64e6du + 0x3039u;
            const uint32_t b = a * 0x41c64e6du + 0x3039u;
            t = (a >> 16) | (b & 0x7fff0000u);
            seed = b;
        }
        Bytes out;
        out.reserve(file.size() > 4u ? file.size() - 4u : 0u);
        size_t i = 0u, j = 0x67u;
        for (size_t o = 4u; o < file.size(); ++o)
        {
            table[i] ^= table[j];
            out.push_back(static_cast<uint8_t>(file[o] ^ (table[i] & 0xffu)));
            i = i + 1u < 0xf9u ? i + 1u : 0u;
            j = j + 1u < 0xf9u ? j + 1u : 0u;
        }
        return out;
    }

    // The minimal standard generator, key = key * 16807 mod 2^31 - 1 by
    // Schrage's method, its low byte XORed into each.
    Bytes parkMiller(const Bytes &file)
    {
        int32_t key = static_cast<int32_t>(seedOf(file));
        Bytes out;
        out.reserve(file.size() > 4u ? file.size() - 4u : 0u);
        for (size_t o = 4u; o < file.size(); ++o)
        {
            const int32_t hi = key / 127773, lo = key % 127773;
            key = 16807 * lo - 2836 * hi;
            if (key <= 0)
                key += 0x7fffffff;
            out.push_back(static_cast<uint8_t>(file[o] ^ (key & 0xff)));
        }
        return out;
    }
}
