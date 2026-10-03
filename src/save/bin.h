#pragma once

// A BinStream's bytes as the games write them: little-endian, a Symbol or
// String as its length then its characters, a fixed name as that many bytes.

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace gh2::save::bin
{
    // Reads to the end or the first fault; `ok` says which.
    struct Reader
    {
        static constexpr uint32_t kMaxString = 256;
        static constexpr uint32_t kMaxCount = 4096;

        const uint8_t *data;
        size_t size;
        size_t at = 0;
        bool ok = true;

        const uint8_t *take(size_t n)
        {
            if (!ok || size - at < n)
            {
                ok = false;
                return nullptr;
            }
            const uint8_t *p = data + at;
            at += n;
            return p;
        }
        uint8_t u8()
        {
            const uint8_t *p = take(1);
            return p ? *p : 0;
        }
        int32_t i32()
        {
            int32_t v = 0;
            if (const uint8_t *p = take(4))
                std::memcpy(&v, p, 4);
            return v;
        }
        uint32_t count()
        {
            const int32_t n = i32();
            if (n < 0 || static_cast<uint32_t>(n) > kMaxCount)
                ok = false;
            return ok ? static_cast<uint32_t>(n) : 0u;
        }
        std::string str()
        {
            const int32_t n = i32();
            if (n < 0 || static_cast<uint32_t>(n) > kMaxString)
                ok = false;
            const uint8_t *p = ok ? take(static_cast<size_t>(n)) : nullptr;
            return p ? std::string(reinterpret_cast<const char *>(p), static_cast<size_t>(n)) : std::string();
        }
        // char[width], to its first NUL if it has one.
        std::string name(size_t width)
        {
            const uint8_t *p = take(width);
            return p ? std::string(p, std::find(p, p + width, uint8_t{0})) : std::string();
        }
    };

    struct Writer
    {
        std::vector<uint8_t> out;

        void u8(uint8_t v) { out.push_back(v); }
        void i32(int32_t v)
        {
            uint8_t b[4];
            std::memcpy(b, &v, 4);
            out.insert(out.end(), b, b + 4);
        }
        void str(const std::string &s)
        {
            i32(static_cast<int32_t>(s.size()));
            out.insert(out.end(), s.begin(), s.end());
        }
        // char[width] holding at most `chars` of the name, NUL padded.
        void name(const std::string &s, size_t width, size_t chars)
        {
            std::vector<uint8_t> field(width, 0);
            std::memcpy(field.data(), s.data(), std::min({s.size(), chars, width}));
            out.insert(out.end(), field.begin(), field.end());
        }
        void bytes(const uint8_t *p, size_t n) { out.insert(out.end(), p, p + n); }
    };
}
