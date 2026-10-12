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
// its file draws (four 360 fonts pack their glyphs another way), and one
// whose twin is a normal map.

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
#include <memory>
#include <set>

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
            return settings::hd(path.starts_with(kTrack) ? settings::kHighway : settings::kUiArt);
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

        // The Tex each Font in a scene draws its glyphs from, through its
        // Mat. None when a Font or its Mat cannot be read.
        std::optional<std::set<std::string>> fontTextures(const milo::Dir &dir)
        {
            std::set<std::string> out;
            for (size_t i = 0; i < dir.entries.size(); ++i)
            {
                if (dir.entries[i].first != "Font")
                    continue;
                const auto name = milo::fontMat(dir.bodies[i], dir.version);
                const auto mat = name ? milo::find(dir, *name) : std::nullopt;
                const auto tex = mat ? milo::matDiffuse(dir.bodies[*mat], dir.version) : std::nullopt;
                if (!tex)
                    return std::nullopt;
                out.insert(*tex);
            }
            return out;
        }

        // The game disc's scene with a stand-in for every Tex that pairs
        // with one in the 360's. None, which leaves the disc's own, when
        // nothing pairs.
        std::optional<ark::Made> makeScene(size_t xbox, const std::string &path, const std::string &twin)
        {
            if (!wanted(path))
                return std::nullopt;
            const auto ours = ark::readFile(0u, path), theirs = ark::readFile(xbox, twin);
            auto scene = ours ? milo::read(*ours) : std::nullopt;
            const auto other = theirs ? milo::read(*theirs) : std::nullopt;
            const auto fonts = scene ? fontTextures(*scene) : std::nullopt;
            if (!other || !fonts || scene->version != 24u || other->version != 25u)
                return std::nullopt;

            size_t paired = 0u;
            for (size_t i = 0; i < scene->entries.size(); ++i)
            {
                const auto &[className, name] = scene->entries[i];
                milo::Bytes &body = scene->bodies[i];
                const auto tex = className == "Tex" ? milo::readTex(body, 24u) : std::nullopt;
                if (!tex || tex->source.empty() || (tex->type & ::milo::tex::kTypeNoPixels) || fonts->contains(name))
                    continue;
                const auto own = oneLevel(body.data() + tex->bitmap, body.size() - tex->bitmap);
                const auto at = std::find(other->entries.begin(), other->entries.end(), scene->entries[i]);
                if (!own || at == other->entries.end())
                    continue;
                const milo::Bytes &theirBody = other->bodies[static_cast<size_t>(at - other->entries.begin())];
                const auto theirTex = milo::readTex(theirBody, 25u);
                if (!theirTex || (theirTex->type & ::milo::tex::kTypeNoPixels) ||
                    !isPicture(theirBody.data() + theirTex->bitmap, theirBody.size() - theirTex->bitmap))
                    continue;
                const std::vector<uint8_t> standIn =
                    textures::standIn(textures::add({xbox, twin, name}), own->width, own->height);
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
            if (!wanted(path))
                return std::nullopt;
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
                const std::string twin = path.substr(0u, path.size() - 3u) + "xbox";
                if (!std::binary_search(theirs.begin(), theirs.end(), twin))
                    continue;
                const auto make = scene ? makeScene : makeBitmap;
                ark::addMade(0u, path, [make, disc = *xbox, path, twin] { return make(disc, path, twin); });
            }
        }
    }
}
