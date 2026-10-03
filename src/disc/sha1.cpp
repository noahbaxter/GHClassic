#include "disc/sha1.h"

#include <array>
#include <cstdio>
#include <vector>

namespace gh2
{
    namespace
    {
        uint32_t rotl(uint32_t x, int n)
        {
            return (x << n) | (x >> (32 - n));
        }

        void block(std::array<uint32_t, 5> &h, const uint8_t *p)
        {
            uint32_t w[80];
            for (int i = 0; i < 16; ++i)
                w[i] = uint32_t(p[i * 4]) << 24 | uint32_t(p[i * 4 + 1]) << 16 | uint32_t(p[i * 4 + 2]) << 8 |
                       uint32_t(p[i * 4 + 3]);
            for (int i = 16; i < 80; ++i)
                w[i] = rotl(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
            uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
            for (int i = 0; i < 80; ++i)
            {
                uint32_t f, k;
                if (i < 20)
                    f = (b & c) | (~b & d), k = 0x5a827999u;
                else if (i < 40)
                    f = b ^ c ^ d, k = 0x6ed9eba1u;
                else if (i < 60)
                    f = (b & c) | (b & d) | (c & d), k = 0x8f1bbcdcu;
                else
                    f = b ^ c ^ d, k = 0xca62c1d6u;
                const uint32_t t = rotl(a, 5) + f + e + k + w[i];
                e = d, d = c, c = rotl(b, 30), b = a, a = t;
            }
            h[0] += a, h[1] += b, h[2] += c, h[3] += d, h[4] += e;
        }
    }

    std::string sha1Hex(const uint8_t *data, size_t size)
    {
        std::array<uint32_t, 5> h{0x67452301u, 0xefcdab89u, 0x98badcfeu, 0x10325476u, 0xc3d2e1f0u};
        size_t whole = size - size % 64;
        for (size_t i = 0; i < whole; i += 64)
            block(h, data + i);

        // The tail, a 1 bit, zeros to 56 mod 64, then the length in bits.
        std::vector<uint8_t> tail(data + whole, data + size);
        tail.push_back(0x80);
        while (tail.size() % 64 != 56)
            tail.push_back(0);
        const uint64_t bits = uint64_t(size) * 8;
        for (int i = 7; i >= 0; --i)
            tail.push_back(uint8_t(bits >> (i * 8)));
        for (size_t i = 0; i < tail.size(); i += 64)
            block(h, tail.data() + i);

        char hex[41];
        for (int i = 0; i < 5; ++i)
            std::snprintf(hex + i * 8, 9, "%08x", h[i]);
        return std::string(hex, 40);
    }
}
