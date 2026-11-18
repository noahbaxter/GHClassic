#include "formats/md5.h"

#include <array>
#include <cstdint>
#include <cstring>

namespace gh2::md5
{
    namespace
    {
        // Each step's shift and its constant, floor(abs(sin(i + 1)) * 2^32).
        constexpr uint32_t kShift[64] = {
            7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20,
            4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21,
        };
        constexpr uint32_t kSine[64] = {
            0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
            0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
            0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
            0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
            0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
            0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
            0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
            0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391,
        };

        uint32_t rotate(uint32_t x, uint32_t by) { return (x << by) | (x >> (32u - by)); }

        void block(std::array<uint32_t, 4> &state, const uint8_t *in)
        {
            uint32_t m[16];
            for (int i = 0; i < 16; ++i)
                m[i] = uint32_t(in[i * 4]) | uint32_t(in[i * 4 + 1]) << 8 | uint32_t(in[i * 4 + 2]) << 16 |
                       uint32_t(in[i * 4 + 3]) << 24;
            uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
            for (uint32_t i = 0; i < 64u; ++i)
            {
                uint32_t f, g;
                if (i < 16u)
                    f = (b & c) | (~b & d), g = i;
                else if (i < 32u)
                    f = (d & b) | (~d & c), g = (5u * i + 1u) % 16u;
                else if (i < 48u)
                    f = b ^ c ^ d, g = (3u * i + 5u) % 16u;
                else
                    f = c ^ (b | ~d), g = (7u * i) % 16u;
                const uint32_t next = d;
                d = c;
                c = b;
                b += rotate(a + f + kSine[i] + m[g], kShift[i]);
                a = next;
            }
            state[0] += a, state[1] += b, state[2] += c, state[3] += d;
        }
    }

    std::string hex(const std::string &bytes)
    {
        std::array<uint32_t, 4> state = {0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476};
        const auto *data = reinterpret_cast<const uint8_t *>(bytes.data());
        size_t whole = bytes.size() / 64u * 64u;
        for (size_t at = 0; at < whole; at += 64u)
            block(state, data + at);
        // The rest, a 1 bit, zeros to 56 bytes of a block, then the length in
        // bits.
        uint8_t tail[128] = {};
        const size_t rest = bytes.size() - whole;
        std::memcpy(tail, data + whole, rest);
        tail[rest] = 0x80u;
        const size_t end = rest < 56u ? 64u : 128u;
        const uint64_t bits = uint64_t(bytes.size()) * 8u;
        for (int i = 0; i < 8; ++i)
            tail[end - 8u + i] = static_cast<uint8_t>(bits >> (8 * i));
        for (size_t at = 0; at < end; at += 64u)
            block(state, tail + at);
        static constexpr char kHex[] = "0123456789abcdef";
        std::string out;
        for (const uint32_t word : state)
            for (int i = 0; i < 4; ++i)
            {
                const auto byte = static_cast<uint8_t>(word >> (8 * i));
                out += kHex[byte >> 4];
                out += kHex[byte & 15u];
            }
        return out;
    }
}
