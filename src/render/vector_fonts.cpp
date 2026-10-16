#include "render/vector_fonts.h"

#include "guest.h"
#include "milo/layout.h"
#include "vector_fonts_pack.h"

#include <miniz.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iostream>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>

namespace gh2::vector_fonts
{
    namespace
    {
        constexpr uint32_t kNone = 0xffffffffu;

        struct Font
        {
            float cell[2] = {};
            std::vector<uint32_t> colors;                    // a layer's rgba8, bottom first
            uint32_t flags = 0;                              // 1: masked, the outline cutting the texture's own pixels
            float tone = 1.0f;                               // its glyphs' usual inside opacity
            float edge = 2.0f;                               // texels inside the outline the tone stands in over
            float feather = 0.0f;                            // texels past it the texture's own fade is kept over
            std::map<uint8_t, std::vector<uint32_t>> glyphs; // a code's glyph layer per layer
        };

        struct Pack
        {
            std::vector<uint32_t> glyphs; // mesh.frag's buffer up to the first table
            std::unordered_map<std::string, size_t> byName;
            std::vector<Font> fonts;
        };

        uint32_t bits(float f)
        {
            uint32_t u;
            std::memcpy(&u, &f, 4u);
            return u;
        }

        // Texels as a table byte, in quarters.
        uint32_t quarters(float texels)
        {
            return static_cast<uint32_t>(std::clamp(texels * 4.0f + 0.5f, 0.0f, 255.0f));
        }

        struct Reader
        {
            const std::vector<uint8_t> &b;
            size_t at = 0;
            bool ok = true;

            template <typename T>
            T get()
            {
                T v{};
                if (at + sizeof(T) > b.size())
                {
                    ok = false;
                    return v;
                }
                std::memcpy(&v, b.data() + at, sizeof(T));
                at += sizeof(T);
                return v;
            }
            std::string name()
            {
                const uint8_t n = get<uint8_t>();
                if (at + n > b.size())
                {
                    ok = false;
                    return {};
                }
                std::string s(reinterpret_cast<const char *>(b.data() + at), n);
                at += n;
                return s;
            }
        };

        // The buffer starts with where its glyph layers, bands and lines
        // start, in uints; a glyph layer is its bounds, band y0 and height,
        // first band and band count; a band its first line and line count;
        // a line two uints of two int16s each, in 1/kLineUnit pixels.
        Pack readPack()
        {
            Pack pack;
            const uint8_t *data = kVectorFontsPack;
            const size_t size = sizeof(kVectorFontsPack);
            uint32_t raw = 0;
            if (size < 8u || std::memcmp(data, "GHVZ", 4u) != 0)
                return pack;
            std::memcpy(&raw, data + 4, 4u);
            std::vector<uint8_t> body(raw);
            mz_ulong got = raw;
            if (mz_uncompress(body.data(), &got, data + 8, static_cast<mz_ulong>(size - 8u)) != MZ_OK || got != raw)
            {
                std::cerr << "[vector fonts] the pack does not inflate" << std::endl;
                return pack;
            }
            Reader r{body};
            const uint32_t magic = r.get<uint32_t>(), version = r.get<uint32_t>(), count = r.get<uint32_t>();
            if (std::memcmp(&magic, "GHVF", 4u) != 0 || version != 3u)
                return pack;
            std::vector<Font> fonts(count);
            for (Font &font : fonts)
            {
                const std::string name = r.name();
                pack.byName[name] = static_cast<size_t>(&font - fonts.data());
                const uint8_t aliases = r.get<uint8_t>();
                for (uint8_t i = 0; i < aliases; ++i)
                    pack.byName[r.name()] = static_cast<size_t>(&font - fonts.data());
                font.cell[0] = r.get<float>();
                font.cell[1] = r.get<float>();
                const uint8_t layers = r.get<uint8_t>();
                for (uint8_t i = 0; i < layers; ++i)
                    font.colors.push_back(r.get<uint32_t>());
                font.flags = r.get<uint8_t>();
                font.tone = r.get<float>();
                font.edge = r.get<float>();
                font.feather = r.get<float>();
                const uint16_t glyphs = r.get<uint16_t>();
                for (uint16_t g = 0; g < glyphs; ++g)
                {
                    const uint16_t code = r.get<uint16_t>();
                    std::vector<uint32_t> per(layers);
                    for (uint32_t &index : per)
                        index = r.get<uint32_t>();
                    if (code < 256u)
                        font.glyphs[static_cast<uint8_t>(code)] = std::move(per);
                }
            }
            std::vector<uint32_t> &out = pack.glyphs;
            out.assign(3u, 0u);
            out[0] = 3u;
            const uint32_t glyphLayers = r.get<uint32_t>();
            for (uint32_t i = 0; i < glyphLayers * 8u; ++i)
                out.push_back(r.get<uint32_t>());
            out[1] = static_cast<uint32_t>(out.size());
            const uint32_t bands = r.get<uint32_t>();
            for (uint32_t i = 0; i < bands * 2u; ++i)
                out.push_back(r.get<uint32_t>());
            out[2] = static_cast<uint32_t>(out.size());
            const uint32_t lines = r.get<uint32_t>();
            for (uint32_t i = 0; i < lines * 2u; ++i)
                out.push_back(r.get<uint32_t>());
            if (!r.ok)
            {
                std::cerr << "[vector fonts] the pack is cut short" << std::endl;
                return Pack{};
            }
            pack.fonts = std::move(fonts);
            std::cerr << "[vector fonts] " << pack.fonts.size() << " fonts, " << glyphLayers << " glyph layers, "
                      << lines << " lines" << std::endl;
            return pack;
        }

        const Pack &pack()
        {
            static const Pack p = readPack();
            return p;
        }

        std::mutex s_tablesLock;
        std::vector<std::vector<uint32_t>> s_tables; // under s_tablesLock
        std::unordered_map<uint32_t, uint32_t> s_byTex;      // game thread
        std::map<std::pair<uint32_t, uint32_t>, uint32_t> s_made; // (font, tex), game thread

        // A Font object's name less ".font".
        std::string fontName(uint8_t *rdram, uint32_t font)
        {
            const uint32_t at = load<uint32_t>(rdram, font + milo::object::kName);
            if (at == 0u)
                return {};
            std::string name(reinterpret_cast<const char *>(getMemPtr(rdram, at)));
            if (name.ends_with(".font"))
                name.resize(name.size() - 5u);
            return name;
        }

        // The table: its entry count, layer count, the scale from a cell's
        // uv to the pack's pixels, each layer's colour, its flags (the
        // second byte its edge, the third its feather, in quarter texels)
        // and tone,
        // then per character its quad's uv rectangle, its cell's uv origin
        // and its glyph layer per layer (two words, kNone past the font's
        // layers).
        std::optional<std::vector<uint32_t>> makeTable(uint8_t *rdram, uint32_t font, uint32_t tex, const Font &packed)
        {
            // RndFont::GetTexCoords (0x1f9c00) reads the texture owner's
            // character info.
            uint32_t owner = font;
            for (int i = 0; i < 4; ++i)
            {
                const uint32_t next = load<uint32_t>(rdram, owner + milo::font::kTexOwner);
                if (next == 0u || next == owner)
                    break;
                owner = next;
            }
            const float cellUv[2] = {load<float>(rdram, owner + milo::font::kTexCell),
                                     load<float>(rdram, owner + milo::font::kTexCell + 4u)};
            if (!(cellUv[0] > 0.0f) || !(cellUv[1] > 0.0f) || packed.colors.size() > 2u)
                return std::nullopt;
            const bool isPacked = load<uint32_t>(rdram, owner + milo::font::kPacked) != 0u;

            // A grid font's cells go in its characters' order, each where
            // the last ended or, when it would pass the bitmap's width, at
            // the next row's start (UpdateChars 0x1fa068). In uv: the cell
            // in pixels is not the one the CharInfo was laid out by once the
            // 360's glyphs have been taken (gh2x/textures.cpp takeFont).
            std::unordered_map<uint8_t, std::pair<float, float>> origin;
            if (!isPacked)
            {
                const uint32_t chars = load<uint32_t>(rdram, owner + milo::font::kChars);
                if (chars == 0u)
                    return std::nullopt;
                const int columns = static_cast<int>(1.0f / cellUv[0] + 1e-3f);
                int i = 0;
                for (const uint8_t *c = getMemPtr(rdram, chars); *c != 0u; ++c, ++i)
                    origin.try_emplace(*c, static_cast<float>(i % columns) * cellUv[0],
                                       static_cast<float>(i / columns) * cellUv[1]);
            }

            std::vector<uint32_t> t = {0u, static_cast<uint32_t>(packed.colors.size()),
                                       bits(packed.cell[0] / cellUv[0]), bits(packed.cell[1] / cellUv[1]),
                                       packed.colors.empty() ? 0u : packed.colors[0],
                                       packed.colors.size() > 1u ? packed.colors[1] : 0u,
                                       packed.flags | quarters(packed.edge) << 8u | quarters(packed.feather) << 16u,
                                       bits(packed.tone)};
            for (const auto &[code, layers] : packed.glyphs)
            {
                const uint32_t info = owner + milo::font::kCharInfo + code * 16u;
                const float u = load<float>(rdram, info), v = load<float>(rdram, info + 4u);
                const float w = load<float>(rdram, info + 8u);
                if (!(w > 0.0f))
                    continue;
                float ox = u, oy = v;
                if (!isPacked)
                {
                    const auto at = origin.find(code);
                    if (at == origin.end())
                        continue;
                    ox = at->second.first;
                    oy = at->second.second;
                    // The row is the game's own v: a cell elsewhere means a
                    // layout this does not know, so the texture draws it.
                    if (std::abs(oy - v) > 1e-3f || u < ox - 1e-3f || u > ox + cellUv[0] + 1e-3f)
                        continue;
                }
                t.insert(t.end(), {bits(u), bits(v), bits(u + w * cellUv[0]), bits(v + cellUv[1]), bits(ox), bits(oy),
                                   layers.empty() ? kNone : layers[0], layers.size() > 1u ? layers[1] : kNone});
                ++t[0];
            }
            if (t[0] == 0u)
                return std::nullopt;
            return t;
        }
    }

    std::optional<uint32_t> noteText(uint8_t *rdram, uint32_t font, uint32_t tex)
    {
        const auto made = s_made.find({font, tex});
        if (made != s_made.end())
            return made->second == kNone ? std::nullopt : std::optional(made->second);
        uint32_t id = kNone;
        const Pack &p = pack();
        const std::string name = fontName(rdram, font);
        const auto found = p.byName.find(name);
        if (found != p.byName.end())
            if (auto table = makeTable(rdram, font, tex, p.fonts[found->second]))
            {
                std::lock_guard lock(s_tablesLock);
                id = static_cast<uint32_t>(s_tables.size());
                s_tables.push_back(std::move(*table));
                std::cerr << "[vector fonts] " << name << " through tex " << std::hex << tex << std::dec << ", "
                          << s_tables.back()[0] << " characters" << std::endl;
            }
        s_made[{font, tex}] = id;
        if (id != kNone)
            s_byTex[tex] = id;
        return id == kNone ? std::nullopt : std::optional(id);
    }

    std::optional<uint32_t> tableOf(uint32_t tex)
    {
        const auto found = s_byTex.find(tex);
        return found != s_byTex.end() ? std::optional(found->second) : std::nullopt;
    }

    void forget(uint32_t tex)
    {
        s_byTex.erase(tex);
        std::erase_if(s_made, [tex](const auto &entry) { return entry.first.second == tex; });
    }

    void forgetAll()
    {
        s_byTex.clear();
        s_made.clear();
    }

    const std::vector<uint32_t> &glyphs()
    {
        return pack().glyphs;
    }

    void tablesSince(size_t known, std::vector<std::vector<uint32_t>> &out)
    {
        std::lock_guard lock(s_tablesLock);
        for (size_t i = known; i < s_tables.size(); ++i)
            out.push_back(s_tables[i]);
    }
}
