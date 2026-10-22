// The track, the HUD and the menus are the same scenes on both discs, the
// same meshes, materials and object names, with the 360's pictures 1 to 8
// times the PS2's size. The PS2 scene stays: when the game first asks for
// one, each Tex's bitmap that the 360's file has under the same name is
// swapped for a stand-in naming the 360's, which the host draws in its place
// (render/texture_store.h). Bitmap files (fretboards, store art) pair by
// path. A stand-in is 4 bits a pixel, which PsTex::SyncBitmap takes from the
// Tex's own word (0x1a1a4c), so that is set too.
//
// Left as they are: a Tex with no pixels of its own (rendered, movie, frame
// buffer), one with no source picture (filled at run time), one a Font in
// its file draws unless every such Font takes the 360's glyphs with it
// (takeFont), one whose twin is a normal map, and one whose twin is laid
// out for a scene the PS2's is not (kOwnLayout).

#include "gh2x/textures.h"

#include "content/games.h"
#include "disc/ark.h"
#include "formats/bitmap.h"
#include "milo/layout.h"
#include "milo/milo.h"
#include "milo/objects.h"
#include "render/font_bake.h"
#include "render/texture_store.h"
#include "render/vector_fonts.h"
#include "settings/settings.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <iostream>
#include <map>
#include <memory>

namespace gh2
{
    namespace
    {
        constexpr const char *kTrack = "track/";
        constexpr const char *kFolders[] = {kTrack, "hud/", "ui/", "system/run/ui/"};

        // The 360's store has a seventh category, Downloads
        // (st_downloads.btn), and its planks are seven where the PS2's
        // six buttons stand on six.
        constexpr const char *kOwnLayout[] = {"us_button_bg.tex"};

        // The track's pictures and fretboards go with the highway, the rest
        // with the HUD and menus.
        bool wanted(const std::string &path)
        {
            return settings::hd(path.starts_with(kTrack) ? settings::kLookHighway : settings::kLookUiArt);
        }

        // A 360 bitmap that is a picture: DXT1 or DXT5, not empty.
        bool isPicture(const uint8_t *data, size_t size)
        {
            const auto header = bitmap::read(data, size);
            return header && header->width != 0u && header->height != 0u &&
                   (header->encoding == bitmap::kDxt1 || header->encoding == bitmap::kDxt5);
        }

        // A PS2 bitmap that is one level and nothing more, as every one in
        // these scenes is.
        std::optional<bitmap::Header> oneLevel(const uint8_t *data, size_t size)
        {
            const auto header = bitmap::read(data, size);
            const size_t bytes = header ? bitmap::kHeaderBytes + bitmap::paletteBytes(*header) +
                                              static_cast<size_t>(header->rowBytes) * header->height
                                        : 0u;
            return header && header->mips == 0u && header->width != 0u && bytes == size ? header : std::nullopt;
        }

        // A character the 360's Font gains: its grid cell, in the pixels of
        // the sheet its Font names, and the traced glyph that fills it.
        struct Added
        {
            float x = 0.0f, y = 0.0f, w = 0.0f, h = 0.0f;
            uint32_t layer = 0;
            float sx = 0.0f, sy = 0.0f; // the pack's pixels a sheet pixel spans
        };

        // What a 360 Font gained (widen): its sheet, the size its Font names,
        // grows `larger` times each way for the cells added.
        struct Widened
        {
            uint32_t larger = 1u;
            float width = 0.0f, height = 0.0f;
            std::vector<Added> cells;
        };

        // The letters and digits our Font has and theirs lacks (the 360 HUD's
        // rokk has no 0 or 3 to 9) added to theirs from the traced font, in
        // free cells of its grid, each measured as Harmonix's tool measured
        // the rest (SetCharInfo 0x1f9c88: the inked columns, width and advance
        // alike). A full grid grows both ways, its old cells keeping the top
        // left: the game sizes a glyph by its cell's shape in uv. A packed or
        // monospace Font is left as it is.
        Widened widen(const milo::Bytes &ours, milo::Bytes &theirs, const std::string &name)
        {
            const auto own = milo::readFont(ours, 24u), their = milo::readFont(theirs, 25u);
            if (!own || !their || !their->infoAt || their->packed || theirs[their->bitmapAt - 2u] != 0u)
                return {};
            std::string missing;
            for (const char c : own->chars)
            {
                const bool alnum = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
                if (alnum && their->chars.find(c) == std::string::npos && missing.find(c) == std::string::npos &&
                    vector_fonts::glyph(name, static_cast<uint8_t>(c)))
                    missing += c;
            }
            if (missing.empty())
                return {};

            uint32_t size[2];
            float cellUv[2];
            std::memcpy(size, theirs.data() + their->bitmapAt, 8u);
            std::memcpy(cellUv, theirs.data() + their->bitmapAt + 8u, 8u);
            const int columns = cellUv[0] > 0.0f ? static_cast<int>(1.0f / cellUv[0] + 1e-3f) : 0;
            if (columns <= 0 || !(cellUv[1] > 0.0f) || size[0] == 0u || size[1] == 0u)
                return {};
            const float cellW = cellUv[0] * static_cast<float>(size[0]), cellH = cellUv[1] * static_cast<float>(size[1]);
            const int rows = static_cast<int>(1.0f / cellUv[1] + 1e-3f);
            // The cells in use: each character's holds its u, v.
            std::vector<std::pair<int, int>> used;
            for (const char c : their->chars)
            {
                float u, v;
                std::memcpy(&u, theirs.data() + their->infoAt + static_cast<uint8_t>(c) * 16u, 4u);
                std::memcpy(&v, theirs.data() + their->infoAt + static_cast<uint8_t>(c) * 16u + 4u, 4u);
                used.emplace_back(static_cast<int>(u / cellUv[0] + 1e-3f), static_cast<int>(v / cellUv[1] + 1e-3f));
            }
            Widened out{1u, static_cast<float>(size[0]), static_cast<float>(size[1]), {}};
            std::vector<std::pair<int, int>> free;
            for (; out.larger <= 4u; out.larger *= 2u)
            {
                free.clear();
                const int across = columns * static_cast<int>(out.larger), down = rows * static_cast<int>(out.larger);
                for (int y = 0; y < down && free.size() < missing.size(); ++y)
                    for (int x = 0; x < across && free.size() < missing.size(); ++x)
                        if (std::find(used.begin(), used.end(), std::pair(x, y)) == used.end())
                            free.emplace_back(x, y);
                if (free.size() == missing.size())
                    break;
            }
            if (free.size() != missing.size())
                return {};
            const float f = static_cast<float>(out.larger);
            const float width = static_cast<float>(size[0]) * f, height = static_cast<float>(size[1]) * f;

            // The string gains the characters, everything after it moving
            // along; the bitmap grows, every u and v and the cell in uv
            // shrinking with it.
            const size_t end = their->charsAt + 4u + their->chars.size(), k = missing.size();
            theirs.insert(theirs.begin() + static_cast<std::ptrdiff_t>(end), missing.begin(), missing.end());
            const uint32_t length = static_cast<uint32_t>(their->chars.size() + k);
            std::memcpy(theirs.data() + their->charsAt, &length, 4u);
            const size_t bitmapAt = their->bitmapAt + k, infoAt = their->infoAt + k;
            const uint32_t grown[2] = {size[0] * out.larger, size[1] * out.larger};
            std::memcpy(theirs.data() + bitmapAt, grown, 8u);
            const float cell[2] = {cellW / width, cellH / height};
            std::memcpy(theirs.data() + bitmapAt + 8u, cell, 8u);
            for (size_t code = 0; code < 256u; ++code)
            {
                float uv[2];
                std::memcpy(uv, theirs.data() + infoAt + code * 16u, 8u);
                uv[0] /= f;
                uv[1] /= f;
                std::memcpy(theirs.data() + infoAt + code * 16u, uv, 8u);
            }
            for (size_t i = 0; i < missing.size(); ++i)
            {
                const auto glyph = vector_fonts::glyph(name, static_cast<uint8_t>(missing[i]));
                Added cell{static_cast<float>(free[i].first) * cellW, static_cast<float>(free[i].second) * cellH,
                           cellW, cellH, glyph->layer, glyph->cell[0] / cellW, glyph->cell[1] / cellH};
                // SetCharInfo's measure: the first and last columns with ink.
                const int x0 = static_cast<int>(cell.x), y0 = static_cast<int>(cell.y);
                const int x1 = static_cast<int>(cell.x + cellW), y1 = static_cast<int>(cell.y + cellH);
                const auto cover = vector_fonts::coverage(cell.layer, x0, y0, x1, y1, cell.x, cell.y, cell.sx, cell.sy);
                int left = x1, right = x0 - 1;
                for (int y = y0; y < y1; ++y)
                    for (int x = x0; x < x1; ++x)
                        if (cover[static_cast<size_t>((y - y0) * (x1 - x0) + (x - x0))] > 0.0f)
                            left = std::min(left, x), right = std::max(right, x);
                const float info[4] = {static_cast<float>(left <= right ? left : x0) / width, cell.y / height,
                                       left <= right ? static_cast<float>(right + 1 - left) / cellW : 0.25f,
                                       left <= right ? static_cast<float>(right + 1 - left) / cellW : 0.25f};
                std::memcpy(theirs.data() + infoAt + static_cast<uint8_t>(missing[i]) * 16u, info, 16u);
                out.cells.push_back(cell);
            }
            return out;
        }

        // A packed 360 Font's characters whose box is narrower than their
        // traced glyph (the 360's helveticablack & is as thin as the PS2's)
        // moved to boxes their glyphs' width where the sheet has room, their
        // info pointing there, the glyphs drawn in with the rest (drawAdded).
        void moveNarrow(milo::Bytes &theirs, const std::string &name, Widened &out)
        {
            const auto their = milo::readFont(theirs, 25u);
            if (!their || !their->infoAt || !their->packed)
                return;
            uint32_t size[2];
            float cellUv[2];
            std::memcpy(size, theirs.data() + their->bitmapAt, 8u);
            std::memcpy(cellUv, theirs.data() + their->bitmapAt + 8u, 8u);
            const float W = static_cast<float>(size[0]), H = static_cast<float>(size[1]);
            const float cellW = cellUv[0] * W, cellH = cellUv[1] * H;
            if (!(cellW > 0.0f) || !(cellH > 0.0f))
                return;
            out.width = W;
            out.height = H;
            for (const char c : their->chars)
            {
                const uint8_t code = static_cast<uint8_t>(c);
                const auto glyph = vector_fonts::glyph(name, code);
                float info[4];
                std::memcpy(info, theirs.data() + their->infoAt + code * 16u, 16u);
                if (!glyph || !(info[2] > 0.0f))
                    continue;
                const float sx = glyph->cell[0] / cellW, sy = glyph->cell[1] / cellH;
                if ((glyph->bounds[2] - glyph->bounds[0]) / sx <= info[2] * cellW + 2.0f)
                    continue;
                const int w = static_cast<int>(std::ceil((glyph->bounds[2] + 0.5f) / sx)) + 1, h = static_cast<int>(std::lround(cellH));
                const auto at = freeBox(theirs, *their, static_cast<int>(size[0]), static_cast<int>(size[1]), w, h);
                if (!at)
                    continue;
                const float width = static_cast<float>(w) / cellW;
                const float moved[4] = {static_cast<float>(at->first) / W, static_cast<float>(at->second) / H, width, width};
                std::memcpy(theirs.data() + their->infoAt + code * 16u, moved, 16u);
                out.cells.push_back({static_cast<float>(at->first), static_cast<float>(at->second), static_cast<float>(w),
                                     static_cast<float>(h), glyph->layer, sx, sy});
            }
        }

        // A packed 360 Font gaining the characters `ours` has and it lacks
        // (the 80s' helveticablack's °), each in a box its traced glyph's
        // width where the sheet has room, drawn in with the rest (drawAdded).
        // One with no traced glyph or no room is not added.
        void addPacked(milo::Bytes &theirs, const std::string &ours, const std::string &name, Widened &out)
        {
            auto their = milo::readFont(theirs, 25u);
            if (!their || !their->infoAt || !their->packed)
                return;
            std::string missing;
            for (const char c : ours)
                if (their->chars.find(c) == std::string::npos && missing.find(c) == std::string::npos &&
                    vector_fonts::glyph(name, static_cast<uint8_t>(c)))
                    missing += c;
            if (missing.empty())
                return;
            uint32_t size[2];
            float cellUv[2];
            std::memcpy(size, theirs.data() + their->bitmapAt, 8u);
            std::memcpy(cellUv, theirs.data() + their->bitmapAt + 8u, 8u);
            const float W = static_cast<float>(size[0]), H = static_cast<float>(size[1]);
            const float cellW = cellUv[0] * W, cellH = cellUv[1] * H;
            if (!(cellW > 0.0f) || !(cellH > 0.0f))
                return;
            out.width = W;
            out.height = H;
            for (const char c : missing)
            {
                const uint8_t code = static_cast<uint8_t>(c);
                const auto glyph = vector_fonts::glyph(name, code);
                const float sx = glyph->cell[0] / cellW, sy = glyph->cell[1] / cellH;
                const int w = static_cast<int>(std::ceil((glyph->bounds[2] + 0.5f) / sx)) + 1, h = static_cast<int>(std::lround(cellH));
                const auto at = freeBox(theirs, *their, static_cast<int>(size[0]), static_cast<int>(size[1]), w, h);
                if (!at)
                    continue;
                // The string gains it, everything after moving along one.
                const size_t end = their->charsAt + 4u + their->chars.size();
                theirs.insert(theirs.begin() + static_cast<std::ptrdiff_t>(end), static_cast<uint8_t>(c));
                const uint32_t length = static_cast<uint32_t>(their->chars.size() + 1u);
                std::memcpy(theirs.data() + their->charsAt, &length, 4u);
                their = milo::readFont(theirs, 25u);
                const float width = static_cast<float>(w) / cellW;
                const float info[4] = {static_cast<float>(at->first) / W, static_cast<float>(at->second) / H, width, width};
                std::memcpy(theirs.data() + their->infoAt + code * 16u, info, 16u);
                out.cells.push_back({static_cast<float>(at->first), static_cast<float>(at->second), static_cast<float>(w),
                                     static_cast<float>(h), glyph->layer, sx, sy});
            }
        }

        // A sheet's decoded pixels grown for what its Font gained, and the
        // traced glyphs drawn into the new cells at its own size, in the
        // colour of its ink. No smaller levels: they no longer match.
        void drawAdded(TextureData &data, const Widened &widened)
        {
            const float scale = static_cast<float>(data.width) / widened.width;
            float ink[3] = {0.0f, 0.0f, 0.0f}, inked = 0.0f;
            for (size_t i = 0; i + 3u < data.rgba.size(); i += 4u)
                if (data.rgba[i + 3u] > 230u)
                {
                    for (int c = 0; c < 3; ++c)
                        ink[c] += data.rgba[i + static_cast<size_t>(c)];
                    inked += 1.0f;
                }
            for (float &c : ink)
                c = inked > 0.0f ? c / inked : 255.0f;
            const uint32_t width = data.width * widened.larger, height = data.height * widened.larger;
            std::vector<uint8_t> grown(static_cast<size_t>(width) * height * 4u, 0u);
            for (uint32_t y = 0; y < data.height; ++y)
                std::memcpy(&grown[static_cast<size_t>(y) * width * 4u], &data.rgba[static_cast<size_t>(y) * data.width * 4u],
                            static_cast<size_t>(data.width) * 4u);
            data.rgba = std::move(grown);
            data.width = width;
            data.height = height;
            data.mips.clear();
            for (const Added &cell : widened.cells)
            {
                const int x0 = static_cast<int>(cell.x * scale), y0 = static_cast<int>(cell.y * scale);
                const int x1 = std::min(static_cast<int>((cell.x + cell.w) * scale), static_cast<int>(data.width));
                const int y1 = std::min(static_cast<int>((cell.y + cell.h) * scale), static_cast<int>(data.height));
                const auto cover = vector_fonts::coverage(cell.layer, x0, y0, x1, y1, cell.x * scale, cell.y * scale,
                                                          cell.sx / scale, cell.sy / scale);
                for (int y = y0; y < y1; ++y)
                    for (int x = x0; x < x1; ++x)
                    {
                        uint8_t *p = &data.rgba[(static_cast<size_t>(y) * data.width + static_cast<size_t>(x)) * 4u];
                        for (int c = 0; c < 3; ++c)
                            p[c] = static_cast<uint8_t>(ink[c]);
                        p[3] = static_cast<uint8_t>(std::lround(cover[static_cast<size_t>((y - y0) * (x1 - x0) + (x - x0))] * 255.0f));
                    }
            }
        }

        // A scene's Fonts on one sheet, each with the 360's Font of its name.
        // None when one of them has no such Font, or that one draws through
        // another Mat: the sheet is then left as it is.
        using SheetFonts = std::optional<std::vector<std::pair<size_t, size_t>>>;

        // The Tex each Font in a scene draws its glyphs from, through its
        // Mat. None when a Font or its Mat cannot be read.
        std::optional<std::map<std::string, SheetFonts>> fontTextures(const milo::Dir &dir, const milo::Dir &other)
        {
            std::map<std::string, SheetFonts> out;
            for (size_t i = 0; i < dir.entries.size(); ++i)
            {
                if (dir.entries[i].first != "Font")
                    continue;
                const auto name = milo::fontMat(dir.bodies[i], dir.version);
                const auto mat = name ? milo::find(dir, *name) : std::nullopt;
                const auto tex = mat ? milo::matDiffuse(dir.bodies[*mat], dir.version) : std::nullopt;
                if (!tex)
                    return std::nullopt;
                const auto twin = std::find(other.entries.begin(), other.entries.end(), dir.entries[i]);
                const size_t theirs = static_cast<size_t>(twin - other.entries.begin());
                SheetFonts &fonts = out.try_emplace(*tex, std::vector<std::pair<size_t, size_t>>{}).first->second;
                if (twin == other.entries.end() || milo::fontMat(other.bodies[theirs], other.version) != name)
                    fonts.reset();
                else if (fonts)
                    fonts->emplace_back(i, theirs);
            }
            return out;
        }

        // The PS2's Font with the 360's glyphs: its characters, widths and
        // kerning, and its cell in the PS2 sheet's pixels, which the game
        // still measures by. The 360's sheet is `across` by `down` times it.
        bool takeFont(milo::Bytes &body, const milo::Bytes &theirBody, float across, float down)
        {
            const auto ours = milo::readFont(body, 24u), theirs = milo::readFont(theirBody, 25u);
            if (!ours || !theirs)
                return false;
            const float cell[2] = {theirs->cellWidth / across, theirs->cellHeight / down};
            body.resize(ours->cell);
            body.insert(body.end(), reinterpret_cast<const uint8_t *>(cell), reinterpret_cast<const uint8_t *>(cell) + 8u);
            body.insert(body.end(), theirs->rest.begin(), theirs->rest.end());
            return true;
        }

        // A disc's scene with a stand-in for every Tex that pairs with one in
        // the 360's. None, which leaves the disc's own, when nothing pairs.
        // With `fontsOnly` (the 80s', whose art is its own) only a Font's
        // sheet pairs, and only where the 360's Font keeps every character
        // ours has but those the game disc's Font of its name gives up too
        // (dyingmarker's ’, which no text uses). `paired` takes each payload
        // a stand-in names.
        std::optional<ark::Made> makeScene(size_t disc, size_t xbox, const std::string &path, const std::string &twin,
                                           bool fontsOnly, std::vector<uint32_t> &paired)
        {
            const auto ours = ark::readFile(disc, path), theirs = ark::readFile(xbox, twin);
            auto scene = ours ? milo::read(*ours) : std::nullopt;
            const auto other = theirs ? milo::read(*theirs) : std::nullopt;
            const auto gameFile = fontsOnly ? ark::readFile(0u, path) : std::nullopt;
            const auto game = gameFile ? milo::read(*gameFile) : std::nullopt;
            const auto fonts = scene && other ? fontTextures(*scene, *other) : std::nullopt;
            if (!other || !fonts || scene->version != 24u || other->version != 25u)
                return std::nullopt;

            for (size_t i = 0; i < scene->entries.size(); ++i)
            {
                const auto &[className, name] = scene->entries[i];
                milo::Bytes &body = scene->bodies[i];
                const auto tex = className == "Tex" ? milo::readTex(body, 24u) : std::nullopt;
                const auto font = fonts->find(name);
                if (!tex || tex->source.empty() || (tex->type & ::milo::tex::kTypeNoPixels) ||
                    (font != fonts->end() && !font->second) || (fontsOnly && font == fonts->end()) ||
                    std::find(std::begin(kOwnLayout), std::end(kOwnLayout), name) != std::end(kOwnLayout))
                    continue;
                const auto own = oneLevel(body.data() + tex->bitmap, body.size() - tex->bitmap);
                // A picture with words in it has one Tex a language on the
                // 360, the English one named _eng (setlist_top_eng.tex).
                auto at = std::find(other->entries.begin(), other->entries.end(), scene->entries[i]);
                if (at == other->entries.end() && name.ends_with(".tex"))
                    at = std::find(other->entries.begin(), other->entries.end(),
                                   std::pair<std::string, std::string>("Tex", name.substr(0u, name.size() - 4u) + "_eng.tex"));
                if (!own || at == other->entries.end())
                    continue;
                const milo::Bytes &theirBody = other->bodies[static_cast<size_t>(at - other->entries.begin())];
                const auto theirTex = milo::readTex(theirBody, 25u);
                if (!theirTex || (theirTex->type & ::milo::tex::kTypeNoPixels) ||
                    !isPicture(theirBody.data() + theirTex->bitmap, theirBody.size() - theirTex->bitmap))
                    continue;
                Widened widened;
                if (font != fonts->end())
                {
                    const auto sheet = bitmap::read(theirBody.data() + theirTex->bitmap, theirBody.size() - theirTex->bitmap);
                    const float across = static_cast<float>(sheet->width) / static_cast<float>(own->width);
                    const float down = static_cast<float>(sheet->height) / static_cast<float>(own->height);
                    bool taken = true;
                    std::vector<milo::Bytes> made;
                    for (const auto &[mine, their] : *font->second)
                    {
                        milo::Bytes theirs = other->bodies[their];
                        // A sheet of one Font only: two would share its new rows.
                        if (font->second->size() == 1u)
                        {
                            widened = widen(scene->bodies[mine], theirs, scene->entries[mine].second);
                            moveNarrow(theirs, scene->entries[mine].second, widened);
                            if (const auto mineFont = fontsOnly ? milo::readFont(scene->bodies[mine], 24u) : std::nullopt)
                                addPacked(theirs, mineFont->chars, scene->entries[mine].second, widened);
                        }
                        if (fontsOnly)
                        {
                            const auto mineFont = milo::readFont(scene->bodies[mine], 24u);
                            const auto theirFont = milo::readFont(theirs, 25u);
                            const auto gameAt = game ? std::find(game->entries.begin(), game->entries.end(), scene->entries[mine])
                                                     : std::vector<std::pair<std::string, std::string>>::const_iterator{};
                            const auto gameFont = game && gameAt != game->entries.end()
                                                      ? milo::readFont(game->bodies[static_cast<size_t>(gameAt - game->entries.begin())], 24u)
                                                      : std::nullopt;
                            taken = taken && mineFont && theirFont &&
                                    std::all_of(mineFont->chars.begin(), mineFont->chars.end(), [&](char c) {
                                        return theirFont->chars.find(c) != std::string::npos ||
                                               (gameFont && gameFont->chars.find(c) != std::string::npos);
                                    });
                        }
                        made.push_back(scene->bodies[mine]);
                        taken = taken && takeFont(made.back(), theirs, across, down);
                    }
                    if (!taken)
                        continue;
                    for (size_t f = 0; f < made.size(); ++f)
                        scene->bodies[(*font->second)[f].first] = std::move(made[f]);
                }
                const uint32_t id = textures::add({xbox, twin, at->second});
                if (!widened.cells.empty())
                {
                    textures::addEdit(id, [widened](TextureData &data) { drawAdded(data, widened); });
                    std::cerr << "[gh2x textures] " << twin << " " << name << ": " << widened.cells.size()
                              << " characters added or moved, drawn from the traced font" << std::endl;
                }
                const std::vector<uint8_t> standIn =
                    textures::standIn(id, own->width * widened.larger, own->height * widened.larger);
                const auto shape = bitmap::read(standIn.data(), standIn.size());
                if (!shape)
                    continue;
                const uint32_t bpp = shape->bpp;
                std::memcpy(body.data() + tex->bpp, &bpp, 4u);
                body.resize(tex->bitmap);
                body.insert(body.end(), standIn.begin(), standIn.end());
                paired.push_back(id);
            }
            if (paired.empty())
                return std::nullopt;
            textures::prefetch(paired);
            return ark::held(milo::write(*scene));
        }

        // A stand-in for a bitmap file the 360 has at the same path, its
        // payload in `paired`.
        std::optional<ark::Made> makeBitmap(size_t xbox, const std::string &path, const std::string &twin,
                                            std::vector<uint32_t> &paired)
        {
            const auto ours = ark::readFile(0u, path), theirs = ark::readFile(xbox, twin);
            const auto own = ours ? bitmap::read(ours->data(), ours->size()) : std::nullopt;
            if (!own || !theirs || !isPicture(theirs->data(), theirs->size()))
                return std::nullopt;
            const uint32_t id = textures::add({xbox, twin, {}});
            std::vector<uint8_t> standIn = textures::standIn(id, own->width, own->height);
            if (standIn.empty())
                return std::nullopt;
            paired.push_back(id);
            textures::prefetch(paired);
            return ark::held(std::move(standIn));
        }

        // Whether a made file is served, its pictures read again as the game
        // loads it: made once, it may be loaded again after they were let go.
        // Read while it loads, not as each picture first shows: star power's
        // meter lightning, a streak's digits.
        std::function<bool()> servedAndRead(const std::string &path,
                                            const std::shared_ptr<const std::vector<uint32_t>> &paired)
        {
            return [path, paired]
            {
                if (!wanted(path))
                    return false;
                if (!paired->empty())
                    textures::prefetch(*paired);
                return true;
            };
        }
    }

    void installGh2xTextures()
    {
        const auto xbox = games::disc("gh2x");
        if (!xbox)
            return;
        // Only the 360's file table is read here.
        for (const char *folder : kFolders)
        {
            const std::vector<std::string> theirs = ark::paths(*xbox, folder);
            for (const std::string &path : ark::paths(0u, folder))
            {
                const bool scene = path.ends_with(".milo_ps2");
                if (!scene && !path.ends_with(".bmp_ps2") && !path.ends_with(".png_ps2"))
                    continue;
                // The PS2's og folders are the 360's ng (ui/image/og/gen,
                // the store's posters and album art).
                std::string twin = path.substr(0u, path.size() - 3u) + "xbox";
                if (const size_t og = twin.find("/og/");
                    og != std::string::npos && !std::binary_search(theirs.begin(), theirs.end(), twin))
                    twin.replace(og, 4u, "/ng/");
                if (!std::binary_search(theirs.begin(), theirs.end(), twin))
                    continue;
                const auto paired = std::make_shared<std::vector<uint32_t>>();
                if (scene)
                    ark::addMade(0u, path,
                                 [disc = *xbox, path, twin, paired] { return makeScene(0u, disc, path, twin, false, *paired); },
                                 servedAndRead(path, paired));
                else
                    ark::addMade(0u, path, [disc = *xbox, path, twin, paired] { return makeBitmap(disc, path, twin, *paired); },
                                 servedAndRead(path, paired));
            }
        }
        // The 80s' fonts are GH2's, the same letters, widths and kerning,
        // its scenes as GH2's are: the 360's spacing for them too, where the
        // 360's Font keeps every character. Its art stays its own.
        const auto eighties = games::disc("gh80s");
        if (!eighties)
            return;
        for (const char *folder : {"hud/", "ui/"})
        {
            const std::vector<std::string> theirs = ark::paths(*xbox, folder);
            for (const std::string &path : ark::paths(*eighties, folder))
            {
                const std::string twin = path.substr(0u, path.size() - 3u) + "xbox";
                if (!path.ends_with(".milo_ps2") || !std::binary_search(theirs.begin(), theirs.end(), twin))
                    continue;
                const auto paired = std::make_shared<std::vector<uint32_t>>();
                ark::addMade(*eighties, path,
                             [disc = *eighties, xbox = *xbox, path, twin, paired]
                             { return makeScene(disc, xbox, path, twin, true, *paired); },
                             servedAndRead(path, paired));
            }
        }
    }
}
