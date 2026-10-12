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
// (takeFont), and one whose twin is a normal map.

#include "gh2x/textures.h"

#include "content/games.h"
#include "disc/ark.h"
#include "formats/bitmap.h"
#include "milo/layout.h"
#include "milo/milo.h"
#include "milo/objects.h"
#include "render/texture_store.h"
#include "settings/settings.h"

#include <algorithm>
#include <cstring>
#include <map>

namespace gh2
{
    namespace
    {
        constexpr const char *kTrack = "track/";
        constexpr const char *kFolders[] = {kTrack, "hud/", "ui/", "system/run/ui/"};

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

        // The game disc's scene with a stand-in for every Tex that pairs
        // with one in the 360's. None, which leaves the disc's own, when
        // nothing pairs.
        std::optional<ark::Made> makeScene(size_t xbox, const std::string &path, const std::string &twin)
        {
            const auto ours = ark::readFile(0u, path), theirs = ark::readFile(xbox, twin);
            auto scene = ours ? milo::read(*ours) : std::nullopt;
            const auto other = theirs ? milo::read(*theirs) : std::nullopt;
            const auto fonts = scene && other ? fontTextures(*scene, *other) : std::nullopt;
            if (!other || !fonts || scene->version != 24u || other->version != 25u)
                return std::nullopt;

            size_t paired = 0u;
            for (size_t i = 0; i < scene->entries.size(); ++i)
            {
                const auto &[className, name] = scene->entries[i];
                milo::Bytes &body = scene->bodies[i];
                const auto tex = className == "Tex" ? milo::readTex(body, 24u) : std::nullopt;
                const auto font = fonts->find(name);
                if (!tex || tex->source.empty() || (tex->type & ::milo::tex::kTypeNoPixels) ||
                    (font != fonts->end() && !font->second))
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
                if (font != fonts->end())
                {
                    const auto sheet = bitmap::read(theirBody.data() + theirTex->bitmap, theirBody.size() - theirTex->bitmap);
                    const float across = static_cast<float>(sheet->width) / static_cast<float>(own->width);
                    const float down = static_cast<float>(sheet->height) / static_cast<float>(own->height);
                    bool taken = true;
                    std::vector<milo::Bytes> made;
                    for (const auto &[mine, their] : *font->second)
                    {
                        made.push_back(scene->bodies[mine]);
                        taken = taken && takeFont(made.back(), other->bodies[their], across, down);
                    }
                    if (!taken)
                        continue;
                    for (size_t f = 0; f < made.size(); ++f)
                        scene->bodies[(*font->second)[f].first] = std::move(made[f]);
                }
                const std::vector<uint8_t> standIn =
                    textures::standIn(textures::add({xbox, twin, at->second}), own->width, own->height);
                const auto shape = bitmap::read(standIn.data(), standIn.size());
                if (!shape)
                    continue;
                const uint32_t bpp = shape->bpp;
                std::memcpy(body.data() + tex->bpp, &bpp, 4u);
                body.resize(tex->bitmap);
                body.insert(body.end(), standIn.begin(), standIn.end());
                ++paired;
            }
            return paired ? std::optional(ark::held(milo::write(*scene))) : std::nullopt;
        }

        // A stand-in for a bitmap file the 360 has at the same path.
        std::optional<ark::Made> makeBitmap(size_t xbox, const std::string &path, const std::string &twin)
        {
            const auto ours = ark::readFile(0u, path), theirs = ark::readFile(xbox, twin);
            const auto own = ours ? bitmap::read(ours->data(), ours->size()) : std::nullopt;
            if (!own || !theirs || !isPicture(theirs->data(), theirs->size()))
                return std::nullopt;
            std::vector<uint8_t> standIn = textures::standIn(textures::add({xbox, twin, {}}), own->width, own->height);
            return standIn.empty() ? std::nullopt : std::optional(ark::held(std::move(standIn)));
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
                const auto make = scene ? makeScene : makeBitmap;
                ark::addMade(0u, path, [make, disc = *xbox, path, twin] { return make(disc, path, twin); },
                             [path] { return wanted(path); });
            }
        }
    }
}
