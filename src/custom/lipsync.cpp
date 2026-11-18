#include "custom/lipsync.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <map>
#include <optional>

namespace gh2::custom
{
    namespace
    {
        using enum Viseme;

        // A .voc key: 2 bytes, its second and weight as floats, 8 bytes.
        constexpr size_t kKeyBytes = 18u, kKeyTimeAt = 2u, kKeyTail = 8u;
        static_assert(kKeyTimeAt + 2u * sizeof(float) + kKeyTail == kKeyBytes);

        bool vowel(char c) { return std::string_view("aeiouy").find(c) != std::string_view::npos; }

        // A consonant cluster's shape, by its first letters; none for an h.
        std::optional<Viseme> consonant(const std::string &c)
        {
            if (c.rfind("th", 0) == 0)
                return Though;
            if (c.rfind("ch", 0) == 0 || c.rfind("sh", 0) == 0)
                return Church;
            if (c.rfind("qu", 0) == 0)
                return Wet;
            switch (c[0])
            {
            case 'j': return Church;
            case 'w': return Wet;
            case 'b': case 'm': case 'p': return Bump;
            case 'f': case 'v': return Fave;
            case 'r': return Roar;
            case 's': case 'z': return Size;
            case 't': case 'd': return Told;
            case 'n': case 'l': return New;
            case 'k': case 'g': case 'c': case 'q': case 'x': return Cage;
            default: return std::nullopt;
            }
        }

        Viseme vowelShape(const std::string &v, bool beforeR)
        {
            const auto has = [&](const char *s) { return v.find(s) != std::string::npos; };
            if (beforeR && v.size() == 1u && v[0] != 'a' && v[0] != 'o')
                return Earth; // her, sir, fur
            if (has("ee") || has("ea") || has("ey") || has("ie") || v == "y")
                return Eat;
            if (has("oo") || has("ew") || has("ue") || has("ui"))
                return Wet;
            if (has("o"))
                return Oat;
            if (has("a") || has("u"))
                return Ox;
            return If;
        }

        // One syllable's shapes.
        void shapesOf(const Sung &s, std::vector<Shape> &out)
        {
            std::string letters;
            for (const unsigned char c : s.text)
                if (std::isalpha(c))
                    letters += static_cast<char>(std::tolower(c));
            const double length = std::max(s.end - s.start, 0.1), end = s.start + length;
            size_t first = 0u;
            while (first < letters.size() && !vowel(letters[first]))
                ++first;
            // A 'y' leading a vowel is a consonant ("yeah").
            if (first == 0u && letters.size() > 1u && letters[0] == 'y' && vowel(letters[1]))
                first = 1u;
            size_t last = first;
            while (last < letters.size() && vowel(letters[last]))
                ++last;
            const std::string onset = letters.substr(0, first),
                              nucleus = first < letters.size() ? letters.substr(first, last - first) : "a",
                              coda = last < letters.size() ? letters.substr(last) : "";
            const double edge = std::min(0.08, length * 0.3);
            const auto open = consonant(onset), close = consonant(coda);
            double from = s.start, to = end;
            if (open)
            {
                out.push_back({s.start, s.start + edge, *open, 0.75f});
                from += edge * 0.5;
            }
            if (close)
            {
                out.push_back({end - edge, end, *close, 0.6f});
                to -= edge * 0.5;
            }
            out.push_back({from, to, vowelShape(nucleus, !coda.empty() && coda[0] == 'r'), 0.85f});
        }

        // A reader over a .voc's bytes.
        struct Reader
        {
            const std::vector<uint8_t> &b;
            size_t p = 0u;
            bool ok = true;

            uint32_t u32()
            {
                if (p + 4u > b.size())
                {
                    ok = false;
                    return 0u;
                }
                uint32_t v;
                std::memcpy(&v, b.data() + p, 4u);
                p += 4u;
                return v;
            }
            void skip(size_t n)
            {
                if (p + n > b.size())
                    ok = false;
                else
                    p += n;
            }
            std::string text()
            {
                const uint32_t n = u32();
                const size_t at = p;
                skip(n);
                return ok ? std::string(b.begin() + static_cast<std::ptrdiff_t>(at), b.begin() + static_cast<std::ptrdiff_t>(at + n))
                          : std::string();
            }
        };

        struct Curve
        {
            size_t keyCountAt = 0u, end = 0u; // its key count's byte, and the byte past its keys
            std::string name;
            uint32_t keys = 0u;
        };

        struct Layout
        {
            size_t sizeAt = 0u; // the file's size field
            std::vector<Curve> curves;
        };

        std::optional<Layout> layout(const std::vector<uint8_t> &voc)
        {
            if (voc.size() < 10u || std::memcmp(voc.data(), "FACE", 4u) != 0)
                return std::nullopt;
            Reader r{voc, 10u};
            Layout out;
            r.text();     // the tool's
            r.skip(2u);
            r.text();     // its project's
            r.skip(12u);
            r.text();     // the track's
            r.skip(2u);
            out.sizeAt = r.p;
            r.skip(6u);
            const uint32_t count = r.u32();
            for (uint32_t i = 0; i < count && r.ok; ++i)
            {
                Curve c;
                r.skip(8u);
                c.name = r.text();
                r.skip(8u);
                c.keyCountAt = r.p;
                c.keys = r.u32();
                r.skip(size_t(c.keys) * kKeyBytes);
                c.end = r.p;
                out.curves.push_back(std::move(c));
            }
            if (!r.ok)
                return std::nullopt;
            return out;
        }

        template <class T> void put(std::vector<uint8_t> &out, T v)
        {
            const auto *b = reinterpret_cast<const uint8_t *>(&v);
            out.insert(out.end(), b, b + sizeof v);
        }

        // A viseme's shapes as keys, (second, weight): up and held and
        // down, one starting no sooner than the last ends.
        std::vector<std::pair<float, float>> curveKeys(std::vector<Shape> shapes)
        {
            std::stable_sort(shapes.begin(), shapes.end(), [](const Shape &a, const Shape &b) { return a.start < b.start; });
            std::vector<std::pair<float, float>> keys;
            for (const Shape &s : shapes)
            {
                const double from = keys.empty() ? s.start : std::max(s.start, double(keys.back().first) + 0.001);
                if (s.end - from < 0.03)
                    continue;
                const double ramp = std::min(0.04, (s.end - from) / 3.0);
                for (const auto &[t, w] : {std::pair{from, 0.0f}, std::pair{from + ramp, s.weight},
                                           std::pair{s.end - ramp, s.weight}, std::pair{s.end, 0.0f}})
                    keys.emplace_back(static_cast<float>(t), w);
            }
            return keys;
        }
    }

    std::vector<Shape> mouthShapes(const std::vector<Sung> &sung)
    {
        std::vector<Shape> out;
        for (const Sung &s : sung)
            shapesOf(s, out);
        return out;
    }

    std::vector<uint8_t> withMouth(const std::vector<uint8_t> &blinkTrack, const std::vector<Shape> &shapes)
    {
        const auto in = layout(blinkTrack);
        if (!in)
            return blinkTrack;
        std::map<std::string, std::vector<Shape>> byName;
        for (const Shape &s : shapes)
            byName[kVisemes[size_t(s.viseme)]].push_back(s);
        // The track as it is, but for the keys of the curves the shapes have.
        std::vector<uint8_t> out;
        size_t from = 0u;
        for (const Curve &c : in->curves)
            if (const auto mine = byName.find(c.name); mine != byName.end())
            {
                out.insert(out.end(), blinkTrack.begin() + static_cast<std::ptrdiff_t>(from),
                           blinkTrack.begin() + static_cast<std::ptrdiff_t>(c.keyCountAt));
                const auto keys = curveKeys(mine->second);
                put(out, static_cast<uint32_t>(keys.size()));
                for (const auto &[t, w] : keys)
                {
                    out.insert(out.end(), kKeyTimeAt, 0u);
                    put(out, t);
                    put(out, w);
                    out.insert(out.end(), kKeyTail, 0u);
                }
                from = c.end;
            }
        out.insert(out.end(), blinkTrack.begin() + static_cast<std::ptrdiff_t>(from), blinkTrack.end());
        const uint32_t size = static_cast<uint32_t>(out.size());
        std::memcpy(out.data() + in->sizeAt, &size, 4u);
        return out;
    }

    std::vector<std::pair<float, float>> mouthOpen(const std::vector<Shape> &shapes)
    {
        std::vector<std::pair<float, float>> spans;
        for (const Shape &s : shapes)
            spans.emplace_back(static_cast<float>(s.start), static_cast<float>(s.end));
        std::sort(spans.begin(), spans.end());
        std::vector<std::pair<float, float>> out;
        for (const auto &s : spans)
            if (!out.empty() && s.first - out.back().second < 0.05f)
                out.back().second = std::max(out.back().second, s.second);
            else
                out.push_back(s);
        return out;
    }

    std::vector<std::pair<float, float>> vocMouthOpen(const std::vector<uint8_t> &voc)
    {
        const auto in = layout(voc);
        if (!in)
            return {};
        std::vector<Shape> open;
        for (const Curve &c : in->curves)
        {
            if (std::find(std::begin(kVisemes), std::end(kVisemes), c.name) == std::end(kVisemes))
                continue;
            std::optional<float> from;
            float t = 0.0f;
            for (uint32_t k = 0; k < c.keys; ++k)
            {
                float w;
                const size_t timeAt = c.keyCountAt + 4u + size_t(k) * kKeyBytes + kKeyTimeAt;
                std::memcpy(&t, voc.data() + timeAt, 4u);
                std::memcpy(&w, voc.data() + timeAt + 4u, 4u);
                if (w > 0.33f && !from)
                    from = t;
                else if (w <= 0.33f && from)
                {
                    open.push_back({*from, t, {}, w});
                    from.reset();
                }
            }
            if (from) // open at its last key
                open.push_back({*from, t, {}, 0.0f});
        }
        return mouthOpen(open);
    }

    std::vector<std::pair<std::string, size_t>> vocCurves(const std::vector<uint8_t> &voc)
    {
        std::vector<std::pair<std::string, size_t>> out;
        if (const auto in = layout(voc))
            for (const Curve &c : in->curves)
                out.emplace_back(c.name, c.keys);
        return out;
    }
}
