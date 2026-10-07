#include "gh1/scene.h"

#include "disc/ark.h"

#include <algorithm>
#include <cstring>

namespace gh2::gh1
{
    namespace
    {
        using milo::putStr;
        using milo::putU32;
        using milo::str;
        using milo::u32;
    }

    float f32(const Bytes &b, size_t o)
    {
        float v = 0.0f;
        if (o + 4u <= b.size())
            std::memcpy(&v, b.data() + o, 4u);
        return v;
    }

    void putF32(Bytes &out, float v)
    {
        uint32_t u = 0u;
        std::memcpy(&u, &v, 4u);
        putU32(out, u);
    }

    uint8_t u8(const Bytes &b, size_t o)
    {
        return o < b.size() ? b[o] : 0u;
    }

    void put(Bytes &out, const Bytes &b, size_t from, size_t to)
    {
        to = std::min(to, b.size());
        if (from < to)
            out.insert(out.end(), b.begin() + static_cast<std::ptrdiff_t>(from),
                       b.begin() + static_cast<std::ptrdiff_t>(to));
    }

    std::vector<std::string> names(const Bytes &b, size_t &o)
    {
        std::vector<std::string> out;
        const uint32_t n = u32(b, o);
        o += 4u;
        for (uint32_t i = 0; i < n && o < b.size(); ++i)
            out.push_back(str(b, o));
        return out;
    }

    void putNames(Bytes &out, const std::vector<std::string> &names)
    {
        putU32(out, static_cast<uint32_t>(names.size()));
        for (const std::string &n : names)
            putStr(out, n);
    }

    std::optional<Trans> trans(const Bytes &b, size_t &o)
    {
        if (u32(b, o) != 8u)
            return std::nullopt;
        Trans out;
        out.at = o;
        o += 100u;
        out.children = names(b, o);
        out.constraint = o;
        o += 4u;
        str(b, o);
        o += 1u;
        out.parent = o;
        str(b, o);
        out.end = o;
        return out;
    }

    std::optional<Draw> draw(const Bytes &b, size_t &o)
    {
        if (u32(b, o) != 1u)
            return std::nullopt;
        Draw out;
        out.showing = u8(b, o + 4u) != 0u;
        o += 5u;
        out.children = names(b, o);
        out.sphere = o;
        o += 16u;
        return out;
    }

    std::optional<Anim> anim(const Bytes &b, size_t &o)
    {
        if (u32(b, o) != 0u)
            return std::nullopt;
        Anim out;
        const uint32_t filters = u32(b, o + 4u);
        o += 8u;
        for (uint32_t i = 0; i < filters && o < b.size(); ++i)
        {
            const uint32_t kind = u32(b, o);
            if (kind > 4u)
                return std::nullopt;
            if (kind == 0u)
            {
                out.scale = f32(b, o + 4u);
                out.offset = f32(b, o + 8u);
            }
            else if (kind == 1u)
            {
                out.min = f32(b, o + 4u);
                out.max = f32(b, o + 8u);
                out.loop = u8(b, o + 12u) != 0u;
            }
            o += 12u + (kind == 1u ? 1u : kind == 4u ? 4u : 0u);
        }
        out.children = names(b, o);
        return out;
    }

    void putAnim(Bytes &out)
    {
        putU32(out, 4u);
        putF32(out, 0.0f);
        putU32(out, 1u);
    }

    std::string filterOf(const std::string &name)
    {
        return name + ".filt";
    }

    Bytes filter(const std::string &name, const Anim &of)
    {
        Bytes out;
        putU32(out, 1u);
        putU32(out, 0u);
        putStr(out, {});
        out.push_back(0u);
        putAnim(out);
        putStr(out, name);
        putF32(out, of.scale);
        putF32(out, of.offset);
        // No range is every frame.
        putF32(out, of.min != of.max ? of.min : -1.0e9f);
        putF32(out, of.min != of.max ? of.max : 1.0e9f);
        putU32(out, of.loop ? 1u : 0u);
        putF32(out, 0.0f);
        return out;
    }

    Stages stages(const Bytes &b, size_t o)
    {
        Stages out;
        str(b, o);
        const uint32_t n = u32(b, o);
        o += 4u;
        for (uint32_t i = 0; i < n && o < b.size(); ++i)
        {
            out.at.push_back(o);
            bool keyed = false;
            for (int list = 0; list < 3; ++list)
            {
                keyed |= u32(b, o) != 0u;
                o += 4u + static_cast<size_t>(u32(b, o)) * 16u;
            }
            const uint32_t textures = u32(b, o);
            keyed |= textures != 0u;
            o += 4u;
            for (uint32_t t = 0; t < textures && o < b.size(); ++t)
            {
                str(b, o);
                o += 4u;
            }
            out.keyed.push_back(keyed);
        }
        out.at.push_back(o);
        return out;
    }

    std::string base(const std::string &name)
    {
        return name.substr(0, name.rfind('.'));
    }

    std::string stageOf(const std::string &anim, size_t stage)
    {
        return base(anim) + "_" + std::to_string(stage) + ".mnm";
    }

    Bytes revision(const Bytes &b)
    {
        return Bytes(b.begin(), b.begin() + std::min<std::ptrdiff_t>(4, static_cast<std::ptrdiff_t>(b.size())));
    }

    std::optional<Parts> parts(const std::string &cls, const Bytes &b)
    {
        size_t o = 4u;
        Parts out;
        if (cls == "View")
        {
            auto a = anim(b, o);
            if (!a)
                return std::nullopt;
            out.anim = std::move(*a);
        }
        std::optional<Trans> t;
        std::optional<Draw> d;
        if (cls == "Text" || cls == "Placer")
        {
            d = draw(b, o);
            t = d ? trans(b, o) : std::nullopt;
        }
        else
        {
            t = trans(b, o);
            d = t ? draw(b, o) : std::nullopt;
        }
        if (!t || !d || o > b.size())
            return std::nullopt;
        out.trans = std::move(*t);
        out.draw = std::move(*d);
        out.rest = o;
        return out;
    }

    Bytes strips(const Bytes &b, size_t o)
    {
        str(b, o);
        str(b, o);
        o += 9u;
        o += 4u + static_cast<size_t>(u32(b, o)) * 48u;
        const uint32_t faces = u32(b, o);
        const size_t face = o + 4u;
        o = face + static_cast<size_t>(faces) * 6u;
        const uint32_t groups = u32(b, o);
        const size_t sizes = o + 4u;
        o = sizes + groups;
        Bytes out;
        if (o + 4u != b.size() || u32(b, o) != 0u)
            return out;
        size_t at = face;
        for (uint32_t g = 0; g < groups; ++g)
        {
            const uint32_t size = b[sizes + g];
            putU32(out, size);
            if (size != 0u)
                putU32(out, 3u * size);
            for (uint32_t i = 1u; i < size; ++i)
                putU32(out, 3u * i);
            putU32(out, 3u * size);
            put(out, b, at, at + 6u * size);
            at += 6u * size;
        }
        return out;
    }

    std::vector<std::string> named(const Bytes &b, const std::string &suffix)
    {
        std::vector<std::string> out;
        for (size_t o = 0u; o + 4u + suffix.size() <= b.size(); ++o)
        {
            const uint32_t n = u32(b, o);
            if (n <= suffix.size() || n > 128u || o + 4u + n > b.size() ||
                std::memcmp(b.data() + o + 4u + n - suffix.size(), suffix.data(), suffix.size()) != 0)
                continue;
            out.emplace_back(reinterpret_cast<const char *>(b.data()) + o + 4u, n);
        }
        return out;
    }

    bool holds(const Bytes &b, const std::string &name)
    {
        Bytes key;
        putStr(key, name);
        return std::search(b.begin(), b.end(), key.begin(), key.end()) != b.end();
    }

    std::optional<milo::Dir> loadScene(size_t disc, const std::string &path)
    {
        const auto file = ark::readFile(disc, path);
        return file ? milo::read(*file) : std::nullopt;
    }

    Bytes texWith(const Bytes &tex, const Bytes &bitmap, bool sized)
    {
        size_t head = 16u;
        str(tex, head);
        head += 9u;
        if (tex.size() < head || bitmap.size() < 32u)
            return {};
        Bytes out(tex.begin(), tex.begin() + static_cast<std::ptrdiff_t>(head));
        if (sized)
        {
            const uint32_t size[3] = {static_cast<uint32_t>(bitmap[7] | bitmap[8] << 8),
                                      static_cast<uint32_t>(bitmap[9] | bitmap[10] << 8), bitmap[1]};
            std::memcpy(out.data() + 4u, size, sizeof size);
        }
        out.back() = 0u;
        out.insert(out.end(), bitmap.begin(), bitmap.end());
        return out;
    }
}
