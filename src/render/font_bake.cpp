#include "render/font_bake.h"

#include "disc/ark.h"
#include "formats/bitmap.h"
#include "milo/layout.h"
#include "milo/milo.h"
#include "milo/objects.h"
#include "render/vector_fonts.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iostream>
#include <set>
#include <string>
#include <vector>

namespace gh2
{
    namespace
    {
        constexpr const char *kFolders[] = {"ui/gen/", "hud/gen/"};
        constexpr float kRegistered = 0.7f; // a font's median cell overlapping its trace this much: the trace sits on the sheet
        constexpr float kOwnArt = 0.8f;     // a cell overlapping its own trace this much is its own art
        constexpr float kBetter = 0.2f;     // else, another character's trace overlapping it this much more: that one's art
        constexpr float kSpill = 1.0f;      // sheet pixels a glyph may pass its box's edge by

        // A PS2 sheet's palette and pixels, in the Tex body (RndBitmap's
        // PixelOffset and PaletteOffset, unswizzled: font sheets never are).
        struct Sheet
        {
            bitmap::Header header;
            uint8_t *palette = nullptr;
            uint8_t *pixels = nullptr;

            uint32_t entry(uint32_t index) const
            {
                // The GS keeps an 8bpp CLUT with bits 3 and 4 swapped.
                if (header.bpp == 8u && (header.encoding & ::milo::bitmap::kPs2Alpha))
                {
                    const uint32_t bits = index & 0x18u;
                    if (bits == 0x08u || bits == 0x10u)
                        index ^= 0x18u;
                }
                return index;
            }

            uint32_t index(int x, int y) const
            {
                const uint8_t b = pixels[static_cast<size_t>(y) * header.rowBytes + ((static_cast<size_t>(x) * header.bpp) >> 3)];
                return header.bpp == 8u ? b : (x & 1) ? b >> 4 : b & 0xfu;
            }

            void set(int x, int y, uint32_t i)
            {
                uint8_t &b = pixels[static_cast<size_t>(y) * header.rowBytes + ((static_cast<size_t>(x) * header.bpp) >> 3)];
                if (header.bpp == 8u)
                    b = static_cast<uint8_t>(i);
                else
                    b = (x & 1) ? static_cast<uint8_t>((b & 0x0fu) | (i << 4)) : static_cast<uint8_t>((b & 0xf0u) | i);
            }

            const uint8_t *colour(uint32_t i) const { return palette + entry(i) * 4u; }

            float alpha(uint32_t i) const
            {
                const float a = colour(i)[3];
                return (header.encoding & ::milo::bitmap::kPs2Alpha) ? std::min(a / 128.0f, 1.0f) : a / 255.0f;
            }
        };

        // The Tex body's sheet, if it is one this can draw into.
        std::optional<Sheet> sheetOf(milo::Bytes &body)
        {
            const auto tex = milo::readTex(body, 24u);
            if (!tex || tex->bitmap >= body.size())
                return std::nullopt;
            uint8_t *at = body.data() + tex->bitmap;
            const auto header = bitmap::read(at, body.size() - tex->bitmap);
            if (!header || (header->bpp != 4u && header->bpp != 8u) || (header->encoding & ::milo::bitmap::kSwizzled))
                return std::nullopt;
            const size_t palette = bitmap::kHeaderBytes, pixels = palette + bitmap::paletteBytes(*header);
            if (tex->bitmap + pixels + static_cast<size_t>(header->rowBytes) * header->height > body.size())
                return std::nullopt;
            return Sheet{*header, at + palette, at + pixels};
        }

        // The palette entry nearest `ink` at `alpha`, both premultiplied.
        uint32_t nearest(const Sheet &sheet, const float ink[3], float alpha)
        {
            const uint32_t count = sheet.header.bpp == 8u ? 256u : 16u;
            uint32_t best = 0u;
            float bestErr = 1e9f;
            for (uint32_t i = 0; i < count; ++i)
            {
                const uint8_t *c = sheet.colour(i);
                const float a = sheet.alpha(i);
                float err = (a - alpha) * (a - alpha) * 4.0f;
                for (int k = 0; k < 3; ++k)
                {
                    const float d = c[k] / 255.0f * a - ink[k] * alpha;
                    err += d * d;
                }
                if (err < bestErr)
                    bestErr = err, best = i;
            }
            return best;
        }

        // The colour of the sheet's solid ink, its most used opaque entry.
        void inkOf(const Sheet &sheet, float ink[3])
        {
            std::vector<uint32_t> uses(256u, 0u);
            for (int y = 0; y < sheet.header.height; ++y)
                for (int x = 0; x < sheet.header.width; ++x)
                    ++uses[sheet.index(x, y)];
            uint32_t best = 0u, most = 0u;
            for (uint32_t i = 0; i < uses.size(); ++i)
                if (uses[i] > most && sheet.alpha(i) > 0.9f)
                    most = uses[i], best = i;
            const uint8_t *c = sheet.colour(best);
            for (int k = 0; k < 3; ++k)
                ink[k] = c[k] / 255.0f;
        }

        // A character's box in a sheet, its pixels, and where the pack's
        // glyphs fall in it.
        struct Cell
        {
            int code = 0;
            uint32_t layer = 0;
            float ox = 0.0f, oy = 0.0f; // its cell's corner, sheet pixels
            float sx = 0.0f, sy = 0.0f; // the pack's pixels a sheet pixel spans
            int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
            std::vector<float> cover; // its own glyph's coverage of each pixel
            float own = 0.0f;         // how much the sheet's art overlaps that
            size_t ink = 0u;          // the sheet's inked pixels in it
        };

        // Glyph layer `layer`'s coverage of each of the cell's pixels.
        std::vector<float> coverage(const Cell &cell, uint32_t layer)
        {
            return vector_fonts::coverage(layer, cell.x0, cell.y0, cell.x1, cell.y1, cell.ox, cell.oy, cell.sx, cell.sy);
        }

        // How much the sheet's art in the cell overlaps `cover`, inked
        // pixels over pixels either has; and the sheet's inked pixels there.
        float overlap(const Sheet &sheet, const Cell &cell, const std::vector<float> &cover, size_t &ink)
        {
            size_t both = 0u, either = 0u, k = 0u;
            ink = 0u;
            for (int y = cell.y0; y < cell.y1; ++y)
                for (int x = cell.x0; x < cell.x1; ++x)
                {
                    const bool own = sheet.alpha(sheet.index(x, y)) > 0.5f, traced = cover[k++] > 0.5f;
                    ink += own;
                    both += own && traced;
                    either += own || traced;
                }
            return either ? static_cast<float>(both) / static_cast<float>(either) : 1.0f;
        }

        // A packed font's character moved to a box its glyph's width where
        // the sheet has room, its info pointing there (width and advance
        // alike, as SetCharInfo 0x1f9c88 measures), its glyph drawn in.
        bool moveWide(Sheet &sheet, milo::Bytes &body, const milo::Font &font, const Cell &cell, const vector_fonts::Glyph &glyph,
                      const float ink[3])
        {
            const int W = sheet.header.width, H = sheet.header.height;
            const int w = static_cast<int>(std::ceil((glyph.bounds[2] + 0.5f) / cell.sx)) + 1, h = cell.y1 - cell.y0;
            const auto at = freeBox(body, font, W, H, w, h, [&sheet](int x, int y) { return sheet.alpha(sheet.index(x, y)) > 0.1f; });
            if (!at)
                return false;
            Cell moved = cell;
            moved.ox = static_cast<float>(at->first);
            moved.oy = static_cast<float>(at->second);
            moved.x0 = at->first;
            moved.y0 = at->second;
            moved.x1 = at->first + w;
            moved.y1 = at->second + h;
            const auto cover = coverage(moved, cell.layer);
            for (int y = moved.y0; y < moved.y1; ++y)
                for (int x = moved.x0; x < moved.x1; ++x)
                    sheet.set(x, y, nearest(sheet, ink, cover[static_cast<size_t>((y - moved.y0) * w + (x - moved.x0))]));
            float cellUv[2];
            std::memcpy(cellUv, body.data() + font.bitmapAt + 8u, 8u);
            const float width = static_cast<float>(w) / (cellUv[0] * static_cast<float>(W));
            const float info[4] = {static_cast<float>(at->first) / static_cast<float>(W), static_cast<float>(at->second) / static_cast<float>(H),
                                   width, width};
            std::memcpy(body.data() + font.infoAt + static_cast<size_t>(cell.code) * 16u, info, 16u);
            return true;
        }

        // Each character of a Font whose cell in its sheet draws something
        // other than its traced glyph, drawn over with that glyph. The ones
        // drawn, for the log.
        std::string bakeFont(milo::Dir &scene, size_t index)
        {
            const std::string &name = scene.entries[index].second;
            const auto font = milo::readFont(scene.bodies[index], scene.version);
            const auto mat = milo::fontMat(scene.bodies[index], scene.version);
            const auto matAt = mat ? milo::find(scene, *mat) : std::nullopt;
            const auto texName = matAt ? milo::matDiffuse(scene.bodies[*matAt], scene.version) : std::nullopt;
            const auto texAt = texName ? milo::find(scene, *texName) : std::nullopt;
            if (!font || !texAt || !font->infoAt)
                return {};
            auto sheet = sheetOf(scene.bodies[*texAt]);
            if (!sheet)
                return {};

            milo::Bytes &body = scene.bodies[index];
            const std::string &chars = font->chars;
            const bool packed = font->packed;
            const size_t info = font->infoAt;
            float cellUv[2];
            std::memcpy(cellUv, body.data() + font->bitmapAt + 8u, 8u);
            if (!(cellUv[0] > 0.0f) || !(cellUv[1] > 0.0f))
                return {};
            const float W = sheet->header.width, H = sheet->header.height;
            std::vector<Cell> cells;
            for (const char c : chars)
            {
                const uint8_t code = static_cast<uint8_t>(c);
                const auto glyph = vector_fonts::glyph(name, code);
                float u, v, w;
                std::memcpy(&u, body.data() + info + code * 16u, 4u);
                std::memcpy(&v, body.data() + info + code * 16u + 4u, 4u);
                std::memcpy(&w, body.data() + info + code * 16u + 8u, 4u);
                if (!glyph || !(w > 0.0f))
                    continue;
                Cell cell{code, glyph->layer};
                // A grid font's cell is the one its u falls in, v its row's
                // top (render/vector_fonts.cpp's table); a packed one's
                // starts at its u, v.
                cell.ox = (packed ? u : std::floor(u / cellUv[0] + 1e-3f) * cellUv[0]) * W;
                cell.oy = (packed ? v : std::floor(v / cellUv[1] + 1e-3f) * cellUv[1]) * H;
                if (!packed && std::abs(cell.oy / H - v) > 1e-3f)
                    continue;
                cell.x0 = static_cast<int>(std::lround(u * W));
                cell.y0 = static_cast<int>(std::lround(v * H));
                cell.x1 = std::min(static_cast<int>(std::lround((u + w * cellUv[0]) * W)), static_cast<int>(W));
                cell.y1 = std::min(static_cast<int>(std::lround((v + cellUv[1]) * H)), static_cast<int>(H));
                if (cell.x0 < 0 || cell.y0 < 0 || cell.x1 <= cell.x0 || cell.y1 <= cell.y0)
                    continue;
                // A sheet pixel's point in the pack, as mesh.frag maps it.
                cell.sx = glyph->cell[0] / (cellUv[0] * W);
                cell.sy = glyph->cell[1] / (cellUv[1] * H);
                cell.cover = coverage(cell, cell.layer);
                cell.own = overlap(*sheet, cell, cell.cover, cell.ink);
                cells.push_back(std::move(cell));
            }
            if (cells.empty())
                return {};
            // A trace that does not sit on this sheet's art (one fitted to
            // another platform's cells) is no judge of it.
            std::vector<float> owns;
            for (const Cell &cell : cells)
                owns.push_back(cell.own);
            std::nth_element(owns.begin(), owns.begin() + owns.size() / 2u, owns.end());
            if (owns[owns.size() / 2u] < kRegistered)
                return {};

            float ink[3];
            inkOf(*sheet, ink);
            std::string drawn;
            for (const Cell &cell : cells)
            {
                // Its art is its own unless the cell is empty or holds
                // another of the font's characters (rokk's 5 an Ü).
                bool other = cell.ink == 0u;
                for (int code = 0; code < 256 && !other && cell.own < kOwnArt; ++code)
                {
                    const auto glyph = code != cell.code ? vector_fonts::glyph(name, static_cast<uint8_t>(code)) : std::nullopt;
                    size_t ink2 = 0u;
                    other = glyph && overlap(*sheet, cell, coverage(cell, glyph->layer), ink2) > cell.own + kBetter;
                }
                // Drawn in its box where it fits. A packed font's box
                // narrower than its glyph (GH2's helveticablack & is 11
                // pixels of a stray underscore) moves to where the sheet has
                // room for it.
                const auto glyph = vector_fonts::glyph(name, static_cast<uint8_t>(cell.code));
                const float gx0 = (glyph->bounds[0] + 0.5f) / cell.sx + cell.ox, gx1 = (glyph->bounds[2] + 0.5f) / cell.sx + cell.ox;
                const float gy0 = (glyph->bounds[1] + 0.5f) / cell.sy + cell.oy, gy1 = (glyph->bounds[3] + 0.5f) / cell.sy + cell.oy;
                if (packed && gx1 - gx0 > static_cast<float>(cell.x1 - cell.x0) + 2.0f * kSpill)
                {
                    if (moveWide(*sheet, body, *font, cell, *glyph, ink))
                        drawn += static_cast<char>(cell.code);
                    continue;
                }
                const bool fits = gx0 > cell.x0 - kSpill && gx1 < cell.x1 + kSpill && gy0 > cell.y0 - kSpill && gy1 < cell.y1 + kSpill;
                if (!other || !fits)
                    continue;
                for (int y = cell.y0; y < cell.y1; ++y)
                    for (int x = cell.x0; x < cell.x1; ++x)
                        sheet->set(x, y, nearest(*sheet, ink, cell.cover[static_cast<size_t>((y - cell.y0) * (cell.x1 - cell.x0) + (x - cell.x0))]));
                drawn += static_cast<char>(cell.code);
            }
            return drawn;
        }
    }

    std::optional<std::pair<int, int>> freeBox(const milo::Bytes &body, const milo::Font &font, int W, int H, int w, int h,
                                               const std::function<bool(int, int)> &inked)
    {
        if (!font.infoAt || W <= 0 || H <= 0)
            return std::nullopt;
        float cellUv[2];
        std::memcpy(cellUv, body.data() + font.bitmapAt + 8u, 8u);
        // Every character's box, a pixel apart from its neighbours'.
        struct Box
        {
            int x0, y0, x1, y1;
        };
        std::vector<Box> boxes;
        std::vector<int> xs = {0}, ys = {0};
        for (size_t code = 0; code < 256u; ++code)
        {
            float info[3];
            std::memcpy(info, body.data() + font.infoAt + code * 16u, 12u);
            if (!(info[2] > 0.0f))
                continue;
            const Box b{static_cast<int>(std::floor(info[0] * W)) - 1, static_cast<int>(std::floor(info[1] * H)) - 1,
                        static_cast<int>(std::ceil((info[0] + info[2] * cellUv[0]) * W)) + 1,
                        static_cast<int>(std::ceil((info[1] + cellUv[1]) * H)) + 1};
            boxes.push_back(b);
            xs.push_back(b.x1);
            ys.push_back(b.y0 + 1);
            ys.push_back(b.y1);
        }
        std::sort(ys.begin(), ys.end());
        std::sort(xs.begin(), xs.end());
        for (const int y : ys)
            for (const int x : xs)
            {
                if (x < 0 || y < 0 || x + w > W || y + h > H)
                    continue;
                const bool clear = std::none_of(boxes.begin(), boxes.end(), [&](const Box &b)
                                                { return x < b.x1 && b.x0 < x + w && y < b.y1 && b.y0 < y + h; });
                bool empty = clear;
                for (int yy = y; empty && inked && yy < y + h; ++yy)
                    for (int xx = x; empty && xx < x + w; ++xx)
                        empty = !inked(xx, yy);
                if (empty)
                    return std::pair(x, y);
            }
        return std::nullopt;
    }

    void installFontBake()
    {
        std::set<std::string> traced;
        for (const std::string &name : vector_fonts::names())
            traced.insert(name + ".font");
        for (const char *folder : kFolders)
            for (const std::string &path : ark::paths(0u, folder))
                if (path.ends_with(".milo_ps2"))
                    ark::addFix(0u, path, [path, traced](std::vector<uint8_t> &file)
                                {
                                    auto scene = milo::read(file);
                                    if (!scene || scene->version != 24u)
                                        return;
                                    bool changed = false;
                                    for (size_t i = 0; i < scene->entries.size(); ++i)
                                    {
                                        if (scene->entries[i].first != "Font" || !traced.contains(scene->entries[i].second))
                                            continue;
                                        const std::string drawn = bakeFont(*scene, i);
                                        if (drawn.empty())
                                            continue;
                                        std::cerr << "[font bake] " << path << " " << scene->entries[i].second << ": drew " << drawn
                                                  << std::endl;
                                        changed = true;
                                    }
                                    if (changed)
                                        file = milo::write(*scene);
                                });
    }
}
