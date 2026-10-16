#include "render/vector_fonts.h"

#include "guest.h"
#include "milo/layout.h"
#include "render/texture_capture.h"
#include "vector_fonts_pack.h"

#include <miniz.h>

#include <algorithm>
#include <array>
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
        constexpr size_t kFitChars = 5u; // letters and digits with ink a sheet's fit needs
        constexpr float kNudge = 1.5f;   // sheet pixels a character may move onto its own ink past the fit

        // A character's quad in a sheet and its cell's corner, both in uv,
        // and the pack's glyph layers it draws.
        struct Quad
        {
            uint8_t code = 0;
            float u0 = 0.0f, v0 = 0.0f, u1 = 0.0f, v1 = 0.0f, ox = 0.0f, oy = 0.0f;
            std::vector<uint32_t> layers; // kNone past the font's own
        };

        // Where a profile of coverage first and last reaches half, to a
        // fraction of a pixel, a pixel's centre on its index
        // (tools/fontstrip.py ink_left).
        bool halfEdges(const std::vector<float> &p, float &lo, float &hi)
        {
            const auto first = std::find_if(p.begin(), p.end(), [](float a) { return a >= 0.5f; });
            if (first == p.end())
                return false;
            const auto last = std::find_if(p.rbegin(), p.rend(), [](float a) { return a >= 0.5f; });
            const size_t i = static_cast<size_t>(first - p.begin()), j = p.size() - 1u - static_cast<size_t>(last - p.rbegin());
            lo = i == 0u ? -0.5f : static_cast<float>(i - 1u) + (0.5f - p[i - 1u]) / std::max(p[i] - p[i - 1u], 1e-3f);
            hi = j + 1u == p.size() ? static_cast<float>(j) + 0.5f
                                    : static_cast<float>(j) + (p[j] - 0.5f) / std::max(p[j] - p[j + 1u], 1e-3f);
            return true;
        }

        // How a font's glyphs sit in the sheet the game draws it from: a
        // point q in a character's cell, in the sheet's pixels with a pixel's
        // centre on its index, is the pack's (s * q + d), per axis. Fitted
        // from the sheet's own ink, each letter's and digit's inked box
        // against its glyph's bounds, least squares with outliers dropped once,
        // so a disc whose sheet frames its glyphs otherwise than the one
        // traced (GH1's rokk sits lower in its cells, the PS2's dyingmarker
        // is drawn larger) has them over its own art. Then each measured
        // character nudged onto its own ink, by what the fit leaves its box
        // off by, up to kNudge of the sheet's pixels: a sheet drawn by hand
        // apart from the one traced (the PS2's dyingmarker) has a one-pixel
        // I that the font's fit alone misses. None when too few have ink.
        struct Fit
        {
            float s[2] = {}, d[2] = {};
            std::map<uint8_t, std::array<float, 2>> nudge; // pack pixels, per character
        };
        std::optional<Fit> fitSheet(const TextureData &sheet, const std::vector<Quad> &quads)
        {
            const std::vector<uint32_t> &vf = pack().glyphs;
            const float W = static_cast<float>(sheet.width), H = static_cast<float>(sheet.height);
            std::vector<std::pair<float, float>> pairs[2]; // (sheet, pack) edges, per axis
            std::vector<std::pair<uint8_t, std::array<float, 8>>> boxes; // each one's sheet x0 x1 y0 y1, pack x0 x1 y0 y1
            size_t inked = 0u;
            for (const Quad &q : quads)
            {
                // Letters and digits fit the font; every character with ink is nudged.
                const bool alnum = (q.code >= '0' && q.code <= '9') || (q.code >= 'A' && q.code <= 'Z') || (q.code >= 'a' && q.code <= 'z');
                if (q.layers.empty())
                    continue;
                float bounds[4] = {1e9f, 1e9f, -1e9f, -1e9f};
                for (const uint32_t layer : q.layers)
                {
                    if (layer == kNone)
                        continue;
                    float b[4];
                    std::memcpy(b, &vf[vf[0] + layer * 8u], sizeof(b));
                    bounds[0] = std::min(bounds[0], b[0]);
                    bounds[1] = std::min(bounds[1], b[1]);
                    bounds[2] = std::max(bounds[2], b[2]);
                    bounds[3] = std::max(bounds[3], b[3]);
                }
                if (bounds[0] > bounds[2])
                    continue;
                const int x0 = std::max(static_cast<int>(std::floor(q.u0 * W)), 0), y0 = std::max(static_cast<int>(std::floor(q.v0 * H)), 0);
                const int x1 = std::min(static_cast<int>(std::ceil(q.u1 * W)), static_cast<int>(sheet.width));
                const int y1 = std::min(static_cast<int>(std::ceil(q.v1 * H)), static_cast<int>(sheet.height));
                if (x1 <= x0 || y1 <= y0)
                    continue;
                std::vector<float> across(static_cast<size_t>(x1 - x0), 0.0f), down(static_cast<size_t>(y1 - y0), 0.0f);
                for (int y = y0; y < y1; ++y)
                    for (int x = x0; x < x1; ++x)
                    {
                        const float a = sheet.rgba[(static_cast<size_t>(y) * sheet.width + static_cast<size_t>(x)) * 4u + 3u] / 255.0f;
                        across[static_cast<size_t>(x - x0)] = std::max(across[static_cast<size_t>(x - x0)], a);
                        down[static_cast<size_t>(y - y0)] = std::max(down[static_cast<size_t>(y - y0)], a);
                    }
                float l, r, t, b;
                if (!halfEdges(across, l, r) || !halfEdges(down, t, b))
                    continue;
                // In the cell: its first pixel's centre is 0.
                const float cx = static_cast<float>(x0) - q.ox * W, cy = static_cast<float>(y0) - q.oy * H;
                boxes.push_back({q.code, {cx + l, cx + r, cy + t, cy + b, bounds[0], bounds[2], bounds[1], bounds[3]}});
                if (!alnum)
                    continue;
                pairs[0].insert(pairs[0].end(), {{cx + l, bounds[0]}, {cx + r, bounds[2]}});
                pairs[1].insert(pairs[1].end(), {{cy + t, bounds[1]}, {cy + b, bounds[3]}});
                ++inked;
            }
            if (inked < kFitChars)
                return std::nullopt;
            Fit fit;
            for (int axis = 0; axis < 2; ++axis)
            {
                std::vector<std::pair<float, float>> &p = pairs[axis];
                for (int pass = 0; pass < 2; ++pass)
                {
                    float mq = 0.0f, mp = 0.0f;
                    for (const auto &[q, k] : p)
                        mq += q, mp += k;
                    mq /= static_cast<float>(p.size());
                    mp /= static_cast<float>(p.size());
                    float cov = 0.0f, var = 0.0f;
                    for (const auto &[q, k] : p)
                        cov += (q - mq) * (k - mp), var += (q - mq) * (q - mq);
                    if (!(var > 1e-6f))
                        return std::nullopt;
                    fit.s[axis] = cov / var;
                    fit.d[axis] = mp - fit.s[axis] * mq;
                    if (pass == 1)
                        break;
                    // Once: edges more than three times the median miss off, at least a pack pixel.
                    std::vector<float> miss;
                    for (const auto &[q, k] : p)
                        miss.push_back(std::abs(fit.s[axis] * q + fit.d[axis] - k));
                    std::vector<float> sorted = miss;
                    std::nth_element(sorted.begin(), sorted.begin() + sorted.size() / 2u, sorted.end());
                    const float cut = std::max(3.0f * sorted[sorted.size() / 2u], 1.0f);
                    std::vector<std::pair<float, float>> kept;
                    for (size_t i = 0; i < p.size(); ++i)
                        if (miss[i] <= cut)
                            kept.push_back(p[i]);
                    if (kept.size() < 2u * kFitChars)
                        break;
                    p = std::move(kept);
                }
                if (!(fit.s[axis] > 0.0f))
                    return std::nullopt;
            }
            for (const auto &[code, e] : boxes)
            {
                std::array<float, 2> &n = fit.nudge[code];
                for (int axis = 0; axis < 2; ++axis)
                {
                    const float *sheetEdges = &e[static_cast<size_t>(axis) * 2u], *packEdges = &e[4u + static_cast<size_t>(axis) * 2u];
                    const float off = 0.5f * (packEdges[0] + packEdges[1]) -
                                      (fit.s[axis] * 0.5f * (sheetEdges[0] + sheetEdges[1]) + fit.d[axis]);
                    const float most = kNudge * fit.s[axis];
                    n[static_cast<size_t>(axis)] = std::clamp(off, -most, most);
                }
            }
            return fit;
        }

        std::optional<std::vector<uint32_t>> makeTable(uint8_t *rdram, uint32_t font, uint32_t tex, const Font &packed,
                                                        const TextureData *sheet)
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

            std::vector<Quad> quads;
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
                    // A grid font's cell is the one its u falls in, u its
                    // ink's left edge (UpdateChars 0x1fa068 measured it, its
                    // tool before it), v the row's top. In uv: the cell in
                    // pixels is not the one the CharInfo was laid out by
                    // once the 360's glyphs have been taken (gh2x/textures.cpp).
                    ox = std::floor(u / cellUv[0] + 1e-3f) * cellUv[0];
                    oy = std::floor(v / cellUv[1] + 1e-3f) * cellUv[1];
                    // A v off its row's top is a layout this does not know,
                    // so the texture draws it.
                    if (std::abs(oy - v) > 1e-3f)
                        continue;
                }
                quads.push_back({code, u, v, u + w * cellUv[0], v + cellUv[1], ox, oy, layers});
            }
            if (quads.empty())
                return std::nullopt;

            // The pack's pixels a uv spans, and where in them a cell's corner
            // falls: the master's cell stretched over the sheet's, unless the
            // sheet's own ink says where its glyphs sit. mesh.frag takes
            // p = (uv - origin) * scale - 0.5, so a fit's s and d go into the
            // scale and each origin (Fit).
            float scale[2] = {packed.cell[0] / cellUv[0], packed.cell[1] / cellUv[1]};
            const auto fit = sheet ? fitSheet(*sheet, quads) : std::nullopt;
            if (fit)
            {
                const float size[2] = {static_cast<float>(sheet->width), static_cast<float>(sheet->height)};
                for (int axis = 0; axis < 2; ++axis)
                    scale[axis] = fit->s[axis] * size[axis];
            }
            std::vector<uint32_t> t = {0u, static_cast<uint32_t>(packed.colors.size()), bits(scale[0]), bits(scale[1]),
                                       packed.colors.empty() ? 0u : packed.colors[0],
                                       packed.colors.size() > 1u ? packed.colors[1] : 0u,
                                       packed.flags | quarters(packed.edge) << 8u | quarters(packed.feather) << 16u,
                                       bits(packed.tone)};
            for (const Quad &q : quads)
            {
                float shift[2] = {0.0f, 0.0f};
                if (fit)
                {
                    const auto nudge = fit->nudge.find(q.code);
                    for (int axis = 0; axis < 2; ++axis)
                        shift[axis] = (fit->d[axis] + (nudge != fit->nudge.end() ? nudge->second[static_cast<size_t>(axis)] : 0.0f) +
                                       0.5f - 0.5f * fit->s[axis]) / scale[axis];
                }
                t.insert(t.end(), {bits(q.u0), bits(q.v0), bits(q.u1), bits(q.v1), bits(q.ox - shift[0]), bits(q.oy - shift[1]),
                                   q.layers.empty() ? kNone : q.layers[0], q.layers.size() > 1u ? q.layers[1] : kNone});
                ++t[0];
            }
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
            if (auto table = makeTable(rdram, font, tex, p.fonts[found->second], capturedTexture(rdram, tex).get()))
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

    std::vector<std::string> names()
    {
        std::vector<std::string> out;
        for (const auto &[name, font] : pack().byName)
            out.push_back(name);
        return out;
    }

    std::optional<Glyph> glyph(std::string name, uint8_t code)
    {
        if (name.ends_with(".font"))
            name.resize(name.size() - 5u);
        const Pack &p = pack();
        const auto found = p.byName.find(name);
        if (found == p.byName.end())
            return std::nullopt;
        const Font &font = p.fonts[found->second];
        const auto at = font.glyphs.find(code);
        if (at == font.glyphs.end() || at->second.empty() || at->second[0] == kNone)
            return std::nullopt;
        Glyph g{at->second[0], {font.cell[0], font.cell[1]}};
        std::memcpy(g.bounds, &p.glyphs[p.glyphs[0] + g.layer * 8u], sizeof(g.bounds));
        return g;
    }

    std::vector<float> coverage(uint32_t layer, int x0, int y0, int x1, int y1, float ox, float oy, float sx, float sy)
    {
        constexpr int kSamples = 4; // a pixel's coverage from kSamples by kSamples points
        std::vector<float> out;
        out.reserve(static_cast<size_t>(std::max(x1 - x0, 0) * std::max(y1 - y0, 0)));
        for (int y = y0; y < y1; ++y)
            for (int x = x0; x < x1; ++x)
            {
                int hits = 0;
                for (int j = 0; j < kSamples; ++j)
                    for (int i = 0; i < kSamples; ++i)
                        hits += inside(layer, (x - ox + (i + 0.5f) / kSamples) * sx - 0.5f, (y - oy + (j + 0.5f) / kSamples) * sy - 0.5f);
                out.push_back(static_cast<float>(hits) / (kSamples * kSamples));
            }
        return out;
    }

    bool inside(uint32_t layer, float x, float y)
    {
        const std::vector<uint32_t> &vf = pack().glyphs;
        const auto f = [&vf](uint32_t i)
        {
            float v;
            std::memcpy(&v, &vf[i], 4u);
            return v;
        };
        const uint32_t at = vf[0] + layer * 8u;
        if (x < f(at) || y < f(at + 1u) || x > f(at + 2u) || y > f(at + 3u))
            return false;
        const int bands = static_cast<int>(vf[at + 7u]);
        const int b = std::clamp(static_cast<int>(std::floor((y - f(at + 4u)) / f(at + 5u))), 0, bands - 1);
        const uint32_t band = vf[at + 6u] + static_cast<uint32_t>(b);
        const uint32_t first = vf[vf[1] + band * 2u], count = vf[vf[1] + band * 2u + 1u];
        bool in = false;
        for (uint32_t i = 0; i < count; ++i)
        {
            // Two words, each two i16 coordinates in 1/32 pixels (mesh.frag's vectorLine).
            const uint32_t a = vf[vf[2] + (first + i) * 2u], c = vf[vf[2] + (first + i) * 2u + 1u];
            const float x0 = static_cast<float>(static_cast<int16_t>(a & 0xffffu)) / 32.0f;
            const float y0 = static_cast<float>(static_cast<int16_t>(a >> 16)) / 32.0f;
            const float x1 = static_cast<float>(static_cast<int16_t>(c & 0xffffu)) / 32.0f;
            const float y1 = static_cast<float>(static_cast<int16_t>(c >> 16)) / 32.0f;
            if ((y0 > y) != (y1 > y) && x < x0 + (y - y0) * (x1 - x0) / (y1 - y0))
                in = !in;
        }
        return in;
    }

    void tablesSince(size_t known, std::vector<std::vector<uint32_t>> &out)
    {
        std::lock_guard lock(s_tablesLock);
        for (size_t i = known; i < s_tables.size(); ++i)
            out.push_back(s_tables[i]);
    }
}
