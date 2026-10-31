#include "content/setlist_printout.h"

#include "disc/ark.h"
#include "formats/bitmap.h"
#include "gh1/scene.h"
#include "render/texture_store.h"

#include <algorithm>
#include <cstring>
#include <iostream>
#include <utility>
#include <vector>

namespace gh2::setlist_printout
{
    namespace
    {
        // The custom list's printout's pieces (ui/dta/quickplay.dta), out of
        // sight until it hangs them up, each a quad of the sheet's showing
        // the whole of its texture, its size below:
        //   ghc_printout.mesh  the multiplayer stats' green-bar paper
        //                      (printerpaper1.tex and .mat, copied whole)
        //   ghc_sticker.mesh   the store's barcode sticker, ghc_sticker.tex,
        //                      its own tape taken off (untaped)
        //   ghc_art.mesh       over the sticker's picture, ghc_art.tex: a
        //                      picture the script sets (custom/song_files.h's
        //                      album art)
        // In the sheet's units (a unit about 2.26 pixels at 1080p): the
        // paper as tall as the screen, its texture's 1:2; the sticker's
        // picture where the store's covers fill it, 220 of its 256.
        //   ghc_tape.mesh      a strip of the multiplayer menu's masking
        //                      tape (multi.milo's dl_maskingtape3.tex,
        //                      copied whole), holding the picture up
        constexpr float kPrintout[2] = {230.0f, 478.0f};
        constexpr float kSticker[2] = {138.0f, 138.0f};
        constexpr float kArt[2] = {118.6f, 118.6f};
        constexpr float kTape[2] = {40.0f, 20.0f};
        constexpr const char *kStickerFile = "ui/image/og/gen/us_logo_generic_keep.png_ps2";

        // setlist1.mesh's quad (600 a side, x across and z up, its middle 0)
        // as mesh `name`, `size` wide and tall about its middle, showing its
        // whole texture: none if the quad is not GH2's.
        std::optional<milo::Bytes> quad(const milo::Dir &scene, const std::string &name, const float (&size)[2])
        {
            const auto sheet = milo::find(scene, "setlist1.mesh");
            if (!sheet)
                return std::nullopt;
            milo::Bytes body = scene.bodies[*sheet];
            // Its geometry its own: a mesh names the one whose vertices it
            // draws, before them.
            if (!milo::replaceStr(body, "setlist1.mesh", name))
                return std::nullopt;
            // Its four vertices, 48 bytes each: position, normal, colour, uv.
            constexpr float kFirst[3] = {-300.0f, 0.0f, 300.0f}, kSheet = 600.0f;
            size_t at = 0u;
            for (; at + 4u + 4u * 48u <= body.size(); ++at)
                if (milo::u32(body, at) == 4u && std::memcmp(body.data() + at + 4u, kFirst, sizeof kFirst) == 0)
                    break;
            if (at + 4u + 4u * 48u > body.size())
                return std::nullopt;
            for (size_t v = 0; v < 4u; ++v)
            {
                uint8_t *vertex = body.data() + at + 4u + v * 48u;
                float xz[2];
                std::memcpy(&xz[0], vertex, 4u);
                std::memcpy(&xz[1], vertex + 8u, 4u);
                for (int k = 0; k < 2; ++k)
                    xz[k] *= size[k] / kSheet;
                std::memcpy(vertex, &xz[0], 4u);
                std::memcpy(vertex + 8u, &xz[1], 4u);
            }
            return body;
        }

        // The store's sticker without the tape it has tucked under its
        // corners. Its paper is the four-sided shape its straight edges make,
        // each fitted to the middle three fifths of its side, so a curled
        // corner as grey as the tape stays. What lies past them goes, and
        // inside them what is darker than any of the paper (the tape's
        // core under 100, a curl's shade over 104) reached from outside,
        // with three rings of its grey fringe.
        void untaped(TextureData &data)
        {
            const int w = static_cast<int>(data.width), h = static_cast<int>(data.height);
            if (w < 16 || h < 16 || data.rgba.size() < static_cast<size_t>(w) * h * 4u)
                return;
            const auto at = [&](int x, int y) { return data.rgba.data() + (static_cast<size_t>(y) * w + x) * 4u; };
            const auto lum = [&](int x, int y)
            {
                const uint8_t *p = at(x, y);
                return 0.299f * p[0] + 0.587f * p[1] + 0.114f * p[2];
            };
            const auto sheet = [&](int x, int y) { return at(x, y)[3] > 128 && lum(x, y) > 170.0f; };
            // x = a y + b along a side, or y = a x + b, least squares.
            const auto fit = [&](bool across, bool fromEnd)
            {
                const int n = across ? h : w, along = across ? w : h;
                double sx = 0, sy = 0, sxx = 0, sxy = 0, count = 0;
                for (int i = n / 5; i < n - n / 5; ++i)
                    for (int j = 0; j < along; ++j)
                    {
                        const int k = fromEnd ? along - 1 - j : j;
                        if (across ? sheet(k, i) : sheet(i, k))
                        {
                            sx += i, sy += k, sxx += double(i) * i, sxy += double(i) * k, ++count;
                            break;
                        }
                    }
                const double a = (count * sxy - sx * sy) / std::max(1e-9, count * sxx - sx * sx);
                return std::pair<double, double>{a, (sy - a * sx) / std::max(1.0, count)};
            };
            const auto left = fit(true, false), right = fit(true, true), top = fit(false, false),
                       bottom = fit(false, true);
            const auto corner = [](std::pair<double, double> v, std::pair<double, double> s)
            {
                const double y = (s.first * v.second + s.second) / (1.0 - s.first * v.first);
                return std::pair<double, double>{v.first * y + v.second, y};
            };
            const std::pair<double, double> quad[4] = {corner(left, top), corner(right, top), corner(right, bottom),
                                                       corner(left, bottom)};
            const auto inside = [&](int x, int y)
            {
                for (int i = 0; i < 4; ++i)
                {
                    const auto &a = quad[i], &b = quad[(i + 1) % 4];
                    if ((b.first - a.first) * (y - a.second) - (b.second - a.second) * (x - a.first) < 0.0)
                        return false;
                }
                return true;
            };
            std::vector<uint8_t> gone(static_cast<size_t>(w) * h), keep(gone.size());
            for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x)
                {
                    keep[y * w + x] = inside(x, y) && at(x, y)[3] > 128;
                    gone[y * w + x] = !keep[y * w + x];
                }
            // Grow `from` into its neighbours where `into` lets it, `times`
            // times, or until it stops with none.
            const auto grow = [&](std::vector<uint8_t> &from, float darkerThan, int times)
            {
                for (int t = 0; t != times; ++t)
                {
                    std::vector<uint8_t> next = from;
                    bool more = false;
                    for (int y = 0; y < h; ++y)
                        for (int x = 0; x < w; ++x)
                        {
                            if (from[y * w + x] || lum(x, y) >= darkerThan)
                                continue;
                            const bool by = (x > 0 && from[y * w + x - 1]) || (x + 1 < w && from[y * w + x + 1]) ||
                                            (y > 0 && from[(y - 1) * w + x]) || (y + 1 < h && from[(y + 1) * w + x]);
                            if (by)
                                next[y * w + x] = 1, more = true;
                        }
                    from = std::move(next);
                    if (!more)
                        break;
                }
            };
            grow(gone, 100.0f, -1);
            std::vector<uint8_t> taped(gone.size());
            for (size_t i = 0; i < gone.size(); ++i)
                taped[i] = gone[i] && keep[i];
            grow(taped, 170.0f, 3);
            for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x)
                    if (gone[y * w + x] || taped[y * w + x])
                        at(x, y)[3] = 0;
        }

        // A PS2 bitmap file of 8 bits a pixel, its palette in the GS's CSM1
        // order (entries 8 to 15 of each 32 swapped with 16 to 23) and alpha
        // to 0x80, with untaped's pixels pointed at an entry all clear. As it
        // was if it is not that, or has no clear entry.
        milo::Bytes untapedSticker(milo::Bytes file)
        {
            const auto header = bitmap::read(file.data(), file.size());
            const size_t palette = bitmap::kHeaderBytes, pixels = palette + 256u * 4u;
            if (!header || header->bpp != 8u || header->mips != 0u ||
                file.size() < pixels + size_t(header->rowBytes) * header->height)
                return file;
            const auto entry = [&](uint32_t index)
            {
                const uint32_t at = (index & 0x18u) == 0x08u ? index + 8u : (index & 0x18u) == 0x10u ? index - 8u : index;
                return file.data() + palette + at * 4u;
            };
            int clear = -1;
            for (uint32_t i = 0; i < 256u && clear < 0; ++i)
                if (entry(i)[3] == 0u)
                    clear = static_cast<int>(i);
            if (clear < 0)
                return file;
            TextureData data;
            data.width = header->width;
            data.height = header->height;
            data.rgba.resize(size_t(data.width) * data.height * 4u);
            const auto index = [&](uint32_t x, uint32_t y) -> uint8_t & { return file[pixels + size_t(y) * header->rowBytes + x]; };
            for (uint32_t y = 0; y < data.height; ++y)
                for (uint32_t x = 0; x < data.width; ++x)
                {
                    const uint8_t *e = entry(index(x, y));
                    uint8_t *p = data.rgba.data() + (size_t(y) * data.width + x) * 4u;
                    p[0] = e[0], p[1] = e[1], p[2] = e[2];
                    p[3] = static_cast<uint8_t>(std::min(255u, e[3] * 2u));
                }
            const std::vector<uint8_t> was = data.rgba;
            untaped(data);
            for (uint32_t y = 0; y < data.height; ++y)
                for (uint32_t x = 0; x < data.width; ++x)
                {
                    const size_t a = (size_t(y) * data.width + x) * 4u + 3u;
                    if (data.rgba[a] == 0u && was[a] != 0u)
                        index(x, y) = static_cast<uint8_t>(clear);
                }
            return file;
        }
    }

    void add(milo::Dir &scene)
    {
        const auto tex = milo::find(scene, "setlist1.tex");
        const auto mat = milo::find(scene, "setlist1.mat");
        if (!tex || !mat || milo::find(scene, "ghc_printout.mesh"))
            return;
        // Read from GH2's disc, whichever game's scene this is.
        const auto stats = gh1::loadScene(0u, "ui/gen/endgame_stats_multi.milo_ps2");
        const auto paperTex = stats ? milo::find(*stats, "printerpaper1.tex") : std::nullopt;
        const auto paperMat = stats ? milo::find(*stats, "printerpaper1.mat") : std::nullopt;
        const auto multi = gh1::loadScene(0u, "ui/gen/multi.milo_ps2");
        const auto tapeTex = multi ? milo::find(*multi, "dl_maskingtape3.tex") : std::nullopt;
        auto printout = quad(scene, "ghc_printout.mesh", kPrintout);
        auto sticker = quad(scene, "ghc_sticker.mesh", kSticker);
        auto art = quad(scene, "ghc_art.mesh", kArt);
        auto tape = quad(scene, "ghc_tape.mesh", kTape);
        const auto logo = ark::readFile(0u, kStickerFile);
        milo::Bytes stickerTex = logo ? gh1::texWith(scene.bodies[*tex], untapedSticker(*logo), true) : milo::Bytes();
        // Blank until a picture is set: a stand-in naming none is a
        // bitmap of its own, all clear.
        milo::Bytes artTex = gh1::texWith(scene.bodies[*tex], textures::standIn(0u, kArtStandIn, kArtStandIn), true);
        milo::Bytes stickerMat = scene.bodies[*mat], artMat = scene.bodies[*mat], tapeMat = scene.bodies[*mat];
        if (!paperTex || !paperMat || !tapeTex || !printout || !sticker || !art || !tape || stickerTex.empty() ||
            artTex.empty() || !milo::replaceStr(stickerMat, "setlist1.tex", "ghc_sticker.tex") ||
            !milo::replaceStr(artMat, "setlist1.tex", "ghc_art.tex") ||
            !milo::replaceStr(tapeMat, "setlist1.tex", "ghc_tape.tex"))
        {
            std::cerr << "[setlist] the song list scene, the stats' paper or the multiplayer menu's tape is not "
                         "as GH2's, no printout" << std::endl;
            return;
        }
        milo::add(scene, "Tex", "printerpaper1.tex", milo::Bytes(stats->bodies[*paperTex]));
        milo::add(scene, "Mat", "printerpaper1.mat", milo::Bytes(stats->bodies[*paperMat]));
        milo::add(scene, "Mesh", "ghc_printout.mesh", std::move(*printout));
        milo::add(scene, "Tex", "ghc_sticker.tex", std::move(stickerTex));
        milo::add(scene, "Mat", "ghc_sticker.mat", std::move(stickerMat));
        milo::add(scene, "Mesh", "ghc_sticker.mesh", std::move(*sticker));
        milo::add(scene, "Tex", "ghc_art.tex", std::move(artTex));
        milo::add(scene, "Mat", "ghc_art.mat", std::move(artMat));
        milo::add(scene, "Mesh", "ghc_art.mesh", std::move(*art));
        milo::add(scene, "Tex", "ghc_tape.tex", milo::Bytes(multi->bodies[*tapeTex]));
        milo::add(scene, "Mat", "ghc_tape.mat", std::move(tapeMat));
        milo::add(scene, "Mesh", "ghc_tape.mesh", std::move(*tape));
    }
}
