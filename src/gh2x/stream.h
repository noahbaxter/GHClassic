#pragma once

// Reading a 360 object body field by field and writing the PS2's. A read
// past the end, a revision not the one wanted, or a field that cannot be what
// the layout says (a bool above 1, a string with control bytes) marks the
// reader bad, and a conversion that ends bad is not used: the layouts were
// worked out from the files, so a body that does not fit is one they missed.

#include "milo/milo.h"

#include <cstring>
#include <string>
#include <vector>

namespace gh2::gh2x
{
    using milo::Bytes;

    struct Reader
    {
        const Bytes &b;
        size_t o = 0u;
        bool ok = true;

        bool need(size_t n)
        {
            if (ok && o + n <= b.size())
                return true;
            ok = false;
            return false;
        }

        // The next n bytes.
        Bytes raw(size_t n)
        {
            if (!need(n))
                return {};
            Bytes out(b.begin() + static_cast<std::ptrdiff_t>(o), b.begin() + static_cast<std::ptrdiff_t>(o + n));
            o += n;
            return out;
        }

        void skip(size_t n)
        {
            if (need(n))
                o += n;
        }

        uint8_t u8() { return need(1u) ? b[o++] : 0u; }

        bool flag()
        {
            const uint8_t v = u8();
            ok = ok && v <= 1u;
            return v != 0u;
        }

        uint16_t u16()
        {
            uint16_t v = 0u;
            if (need(2u))
                std::memcpy(&v, b.data() + o, 2u), o += 2u;
            return v;
        }

        uint32_t u32()
        {
            uint32_t v = 0u;
            if (need(4u))
                std::memcpy(&v, b.data() + o, 4u), o += 4u;
            return v;
        }

        float f32()
        {
            const uint32_t u = u32();
            float v;
            std::memcpy(&v, &u, 4u);
            return v;
        }

        std::string str(size_t longest = 256u)
        {
            const uint32_t n = u32();
            if (n > longest || !need(n))
            {
                ok = false;
                return {};
            }
            std::string s(reinterpret_cast<const char *>(b.data() + o), n);
            for (const char c : s)
                ok = ok && ((c >= 32 && c <= 126) || c == 9);
            o += n;
            return s;
        }

        uint32_t count(uint32_t most)
        {
            const uint32_t n = u32();
            ok = ok && n <= most;
            return ok ? n : 0u;
        }

        std::vector<std::string> strs()
        {
            std::vector<std::string> out(count(100000u));
            for (std::string &s : out)
                s = str();
            return ok ? out : std::vector<std::string>{};
        }

        void rev(uint32_t want) { ok = u32() == want && ok; }

        size_t left() const { return o <= b.size() ? b.size() - o : 0u; }

        // The bytes from `from` to here.
        Bytes since(size_t from) const
        {
            return Bytes(b.begin() + static_cast<std::ptrdiff_t>(from), b.begin() + static_cast<std::ptrdiff_t>(o));
        }

        // Whether every byte was read and every field fit.
        bool done() const { return ok && o == b.size(); }
    };

    struct Writer
    {
        Bytes out;

        Writer &u8(uint8_t v) { return out.push_back(v), *this; }
        Writer &u16(uint16_t v) { return raw(&v, 2u); }
        Writer &u32(uint32_t v) { return raw(&v, 4u); }
        Writer &f32(float v) { return raw(&v, 4u); }
        Writer &str(const std::string &s) { return u32(static_cast<uint32_t>(s.size())), raw(s.data(), s.size()); }

        Writer &strs(const std::vector<std::string> &v)
        {
            u32(static_cast<uint32_t>(v.size()));
            for (const std::string &s : v)
                str(s);
            return *this;
        }

        Writer &raw(const void *p, size_t n)
        {
            const auto *bytes = static_cast<const uint8_t *>(p);
            out.insert(out.end(), bytes, bytes + n);
            return *this;
        }

        Writer &raw(const Bytes &b) { return raw(b.data(), b.size()); }
    };
}
