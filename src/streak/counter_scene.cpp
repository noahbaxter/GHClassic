// counter_scene: the streak counter's scene, made from score_display.milo:
// four wheels of its six, white digits on black and the last black on white,
// an icon on the first (GH2's crowd's lighter flame), framed in the score
// shell's metal along its sides and bottom.

#include "streak/counter_scene.h"

#include "formats/bitmap.h"
#include "milo/objects.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

namespace gh2::streak_counter
{
    namespace
    {
        constexpr const char *kScore = "hud/gen/score_display.milo_ps2";
        // streak_counter<n>, for each meter <n>.
        constexpr const char *kCounterDisplay = "streak_counter";

        milo::Bytes lengthPrefixed(const std::string &s)
        {
            milo::Bytes out;
            milo::putStr(out, s);
            return out;
        }

        // `from`, length-prefixed, as `to`. False if the body has none.
        bool replaceStr(milo::Bytes &body, const std::string &from, const std::string &to)
        {
            const milo::Bytes a = lengthPrefixed(from), b = lengthPrefixed(to);
            const auto it = std::search(body.begin(), body.end(), a.begin(), a.end());
            if (it == body.end())
                return false;
            const auto at = body.erase(it, it + static_cast<std::ptrdiff_t>(a.size()));
            body.insert(at, b.begin(), b.end());
            return true;
        }

        // A list of names in a body (a Group's objects, a score display's
        // meshes): where its count is, the list holding `name`. None when
        // no such list holds it.
        std::optional<size_t> listHolding(const milo::Bytes &body, const std::string &name)
        {
            const milo::Bytes member = lengthPrefixed(name);
            const auto it = std::search(body.begin(), body.end(), member.begin(), member.end());
            if (it == body.end())
                return std::nullopt;
            const size_t at = static_cast<size_t>(it - body.begin());
            for (size_t count = at; count >= 4u; --count)
            {
                const uint32_t n = milo::u32(body, count - 4u);
                if (n == 0u || n > 64u)
                    continue;
                size_t o = count;
                bool covers = false;
                for (uint32_t k = 0; k < n && o < body.size(); ++k)
                {
                    covers = covers || o == at;
                    milo::str(body, o);
                }
                if (covers)
                    return count - 4u;
            }
            return std::nullopt;
        }

        // `name` added to the list holding `after`, after it.
        bool addAfter(milo::Bytes &body, const std::string &after, const std::string &name)
        {
            const auto count = listHolding(body, after);
            if (!count)
                return false;
            const milo::Bytes member = lengthPrefixed(after), added = lengthPrefixed(name);
            const auto at = std::search(body.begin(), body.end(), member.begin(), member.end());
            body.insert(at + static_cast<std::ptrdiff_t>(member.size()), added.begin(), added.end());
            const uint32_t n = milo::u32(body, *count) + 1u;
            std::memcpy(body.data() + *count, &n, 4u);
            return true;
        }

        // `name` taken out of the list holding it.
        bool remove(milo::Bytes &body, const std::string &name)
        {
            const auto count = listHolding(body, name);
            if (!count)
                return false;
            const milo::Bytes member = lengthPrefixed(name);
            const auto at = std::search(body.begin(), body.end(), member.begin(), member.end());
            body.erase(at, at + static_cast<std::ptrdiff_t>(member.size()));
            const uint32_t n = milo::u32(body, *count) - 1u;
            std::memcpy(body.data() + *count, &n, 4u);
            return true;
        }

        // A Mat (revision 27)'s colour white: past its type, a byte and its
        // blend. The score's digits are tinted (0, 0.75, 1).
        void whiten(milo::Bytes &mat)
        {
            size_t o = 8u;
            milo::str(mat, o);
            o += 1u + 4u;
            const float white = 1.0f;
            for (size_t ch = 0; ch < 3u && o + 4u * ch + 4u <= mat.size(); ++ch)
                std::memcpy(mat.data() + o + 4u * ch, &white, 4u);
        }

        // Where a palette's entry for an index is: an 8-bit one is stored
        // in the GS's CSM1 order.
        size_t slot(size_t index, uint8_t bpp)
        {
            if (bpp == 8u && (index & 0x18u) == 0x08u)
                return index + 8u;
            if (bpp == 8u && (index & 0x18u) == 0x10u)
                return index - 8u;
            return index;
        }

        // A Tex's colours turned over. The pixels `keep` names (by place and
        // whether they were light) stay as they were, each the nearest
        // turned-over colour.
        using Keep = std::function<bool(size_t x, size_t y, bool light)>;
        void invert(milo::Bytes &body, uint32_t version, const Keep &keep = {})
        {
            const auto tex = milo::readTex(body, version);
            const auto header = tex ? bitmap::read(body.data() + tex->bitmap, body.size() - tex->bitmap) : std::nullopt;
            if (!header)
                return;
            uint8_t *palette = body.data() + tex->bitmap + bitmap::kHeaderBytes;
            const size_t entries = bitmap::paletteBytes(*header) / 4u;
            const std::vector<uint8_t> was(palette, palette + entries * 4u);
            for (size_t c = 0; c < entries * 4u; c += 4u)
                for (size_t ch = 0; ch < 3u; ++ch)
                    palette[c + ch] = static_cast<uint8_t>(255u - palette[c + ch]);
            if (!keep || header->bpp != 8u)
                return;
            uint8_t *pixels = palette + entries * 4u;
            for (size_t y = 0; y < header->height; ++y)
                for (size_t x = 0; x < header->width; ++x)
                {
                    uint8_t &index = pixels[y * header->rowBytes + x];
                    const uint8_t *old = was.data() + slot(index, 8u) * 4u;
                    if (!keep(x, y, old[0] + old[1] + old[2] >= 3u * 128u))
                        continue;
                    uint32_t best = UINT32_MAX;
                    for (size_t k = 0; k < entries; ++k)
                    {
                        uint32_t d = 0u;
                        for (size_t ch = 0; ch < 4u; ++ch)
                        {
                            const int v = int(palette[slot(k, 8u) * 4u + ch]) - int(old[ch]);
                            d += static_cast<uint32_t>(v * v);
                        }
                        if (d < best)
                            best = d, index = static_cast<uint8_t>(k);
                    }
                }
        }

        // The wheels the counter keeps, of the score's six (score_num_1 on
        // the left): an icon, then three digits, the last its ones. Its
        // frame (score_num_frame.tex, 256 wide, the full width of its quad)
        // is cut at the line left of the third wheel.
        constexpr const char *kDropped[] = {"score_num_1.mesh", "score_num_2.mesh"};
        constexpr float kFrameFrom = 81.0f / 256.0f;
        // Its black edge, which stays black: above row 37 and from row 98,
        // the line it is cut at (81 to 84) and its right end (from 243), and
        // the gap after the icon's wheel (118 to 136).
        bool frameKeeps(size_t x, size_t y, bool light)
        {
            if (light)
                return false;
            return y < 37u || y >= 98u || x < 85u || x >= 243u || (x >= 118u && x < 136u);
        }

        // The frame's art, rows 28 to 101, has its corners and edges clear
        // where the score's shell met it; the metal here sits a pixel off,
        // and the venue showed between. Clear pixels from just above the art
        // to just below it are its opaque black instead.
        constexpr size_t kArtFrom = 26u, kArtTo = 106u;
        void closeEdges(milo::Bytes &body, uint32_t version)
        {
            const auto tex = milo::readTex(body, version);
            const auto header = tex ? bitmap::read(body.data() + tex->bitmap, body.size() - tex->bitmap) : std::nullopt;
            if (!header || header->bpp != 8u)
                return;
            const uint8_t *palette = body.data() + tex->bitmap + bitmap::kHeaderBytes;
            const size_t entries = bitmap::paletteBytes(*header) / 4u;
            const auto colour = [&](size_t index) { return palette + slot(index, 8u) * 4u; };
            std::optional<uint8_t> black;
            for (size_t k = 0; k < entries; ++k)
                if (const uint8_t *c = colour(k); c[3] >= 0x80u &&
                    (!black || c[0] + c[1] + c[2] < colour(*black)[0] + colour(*black)[1] + colour(*black)[2]))
                    black = static_cast<uint8_t>(k);
            if (!black)
                return;
            uint8_t *pixels = body.data() + tex->bitmap + bitmap::kHeaderBytes + entries * 4u;
            for (size_t y = kArtFrom; y < std::min<size_t>(kArtTo, header->height); ++y)
                for (size_t x = 0; x < header->width; ++x)
                    if (uint8_t &index = pixels[y * header->rowBytes + x]; colour(index)[3] == 0u)
                        index = *black;
        }

        // Where a one-quad Mesh 28's verts are: their count's place. A
        // count, then 48 bytes each (a place, a normal, a colour and a uv),
        // then a count of faces of three shorts.
        std::optional<size_t> quadVerts(const milo::Bytes &mesh)
        {
            for (size_t o = 65u + 48u; o + 8u <= mesh.size(); ++o)
            {
                const size_t end = o + 4u + 4u * 48u;
                if (milo::u32(mesh, o) == 4u && end + 4u <= mesh.size() && milo::u32(mesh, end) == 2u)
                    return o;
            }
            return std::nullopt;
        }

        // The frame quad's verts left of its middle moved to kFrameFrom,
        // their picture with them.
        void trimFrame(milo::Bytes &mesh)
        {
            const auto at = quadVerts(mesh);
            if (!at)
                return;
            for (size_t v = 0; v < 4u; ++v)
            {
                uint8_t *vert = mesh.data() + *at + 4u + v * 48u;
                float x;
                std::memcpy(&x, vert, 4u);
                if (x >= 0.0f)
                    continue;
                x += 2.0f * -x * kFrameFrom;
                std::memcpy(vert, &x, 4u);
                std::memcpy(vert + 40u, &kFrameFrom, 4u);
            }
        }

        // Its frame: the score shell's metal (score_frame.tex in the
        // multiplier's scene, drawn with the shell's own Mat, lit as the
        // shell is), from the shell's bottom edge, the dark grey bevel right
        // above the counter: a straight band from 70 to 190 across, rows 229
        // (inside) to 249 (outside); and its bottom left corner, where that
        // bevel bends up its side, from 22 (outside) to 60 across and 250
        // (outside) to 212 up.
        constexpr const char *kShell = "hud/gen/streak_display.milo_ps2";
        constexpr const char *kMetal = "score_frame.tex", *kMetalMat = "score_frame.mat";
        constexpr float kBandFrom = 70.0f / 256.0f, kBandTo = 190.0f / 256.0f;
        constexpr float kBandOuter = 249.0f / 256.0f, kBandInner = 229.0f / 256.0f;
        constexpr float kCornerOuterU = 22.0f / 256.0f, kCornerInnerU = 60.0f / 256.0f;
        constexpr float kCornerOuterV = 250.0f / 256.0f, kCornerInnerV = 212.0f / 256.0f;
        constexpr float kMetalWidth = 7.0f, kCornerSize = kMetalWidth * 38.0f / 20.0f;

        struct Quad
        {
            float x[4], z[4], u[4], v[4]; // in the order of the frame quad's verts: +x+z, -x+z, -x-z, +x-z
        };

        // A copy of the frame quad's verts as `quad`: places in the display
        // turned into the quad's own by its transform, its scale and place.
        void placeQuad(milo::Bytes &mesh, size_t o, const Quad &quad)
        {
            float local[12];
            std::memcpy(local, mesh.data() + 17u, sizeof local);
            const float sx = local[0], sz = local[8], px = local[9], pz = local[11];
            for (size_t v = 0; v < 4u; ++v)
            {
                uint8_t *vert = mesh.data() + o + 4u + v * 48u;
                const float x = (quad.x[v] - px) / sx, z = (quad.z[v] - pz) / sz;
                std::memcpy(vert, &x, 4u);
                std::memcpy(vert + 8u, &z, 4u);
                std::memcpy(vert + 40u, &quad.u[v], 4u);
                std::memcpy(vert + 44u, &quad.v[v], 4u);
            }
        }

        // The frame's pieces around the trimmed frame quad, open at the top:
        // its top edge is the shell's bottom, which it pulls down out of. A
        // corner each at the bottom, the shell's mirrored, a run between
        // them, and a run up each side, the band turned, to the top.
        std::array<Quad, 5> metalAround(float left, float right, float bottom, float top)
        {
            const float t = kMetalWidth, c = kCornerSize;
            const float a = kBandFrom, b = kBandTo, o = kBandOuter, i = kBandInner;
            const float co = kCornerOuterU, ci = kCornerInnerU, vo = kCornerOuterV, vi = kCornerInnerV;
            const float ol = left - t, orr = right + t, ob = bottom - t;
            return {{
                {{ol + c, ol, ol, ol + c}, {ob + c, ob + c, ob, ob}, {ci, co, co, ci}, {vi, vi, vo, vo}},
                {{orr, orr - c, orr - c, orr}, {ob + c, ob + c, ob, ob}, {co, ci, ci, co}, {vi, vi, vo, vo}},
                {{orr - c, ol + c, ol + c, orr - c}, {bottom, bottom, ob, ob}, {b, a, a, b}, {i, i, o, o}},
                {{left, ol, ol, left}, {top, top, ob + c, ob + c}, {b, b, a, a}, {i, o, o, i}},
                {{orr, right, right, orr}, {top, top, ob + c, ob + c}, {b, b, a, a}, {o, i, i, o}},
            }};
        }

        // The icon: GH2's crowd's lighter flame, 32 square, 8-bit, on its
        // own black, filling as much of a wheel's picture (32 by 64) as a
        // digit does.
        constexpr const char *kLighterScene = "char/crowd/og/gen/lighter_flame.milo_ps2", *kLighter = "lighter_flame_2.tex";
        constexpr size_t kIconWidth = 28u, kIconHeight = 44u;

        // A 4- or 8-bit picture as a wheel's, `width` by `height`: what of
        // it is not its darkest colour, scaled up (each pixel the nearest of
        // its own) as far as fits kIconWidth by kIconHeight, in the middle,
        // on that darkest colour.
        std::optional<milo::Bytes> wheelPicture(const uint8_t *data, size_t size, size_t width, size_t height)
        {
            const auto header = bitmap::read(data, size);
            if (!header || (header->bpp != 4u && header->bpp != 8u))
                return std::nullopt;
            const size_t palette = bitmap::paletteBytes(*header), row = header->rowBytes, bpp = header->bpp;
            if (size < bitmap::kHeaderBytes + palette + row * header->height)
                return std::nullopt;
            const uint8_t *colours = data + bitmap::kHeaderBytes, *pixels = colours + palette;
            const auto at = [&](size_t x, size_t y) -> size_t
            {
                const uint8_t b = pixels[y * row + x * bpp / 8u];
                return bpp == 8u ? b : (b >> (4u * (x & 1u))) & 15u;
            };
            const auto light = [&](size_t k)
            {
                const uint8_t *c = colours + 4u * slot(k, static_cast<uint8_t>(bpp));
                return c[0] + c[1] + c[2];
            };
            size_t black = 0u;
            for (size_t k = 1; k < palette / 4u; ++k)
                if (light(k) < light(black))
                    black = k;
            // Its extent: every pixel brighter than its darkest by a little.
            size_t x0 = header->width, y0 = header->height, x1 = 0u, y1 = 0u;
            for (size_t y = 0; y < header->height; ++y)
                for (size_t x = 0; x < header->width; ++x)
                    if (light(at(x, y)) > light(black) + 24u)
                        x0 = std::min(x0, x), y0 = std::min(y0, y), x1 = std::max(x1, x + 1u), y1 = std::max(y1, y + 1u);
            if (x1 <= x0 || y1 <= y0)
                return std::nullopt;
            const float scale = std::max(1.0f, std::min(float(kIconWidth) / float(x1 - x0), float(kIconHeight) / float(y1 - y0)));
            const size_t w = std::min(width, size_t(float(x1 - x0) * scale)), h = std::min(height, size_t(float(y1 - y0) * scale));
            const size_t left = (width - w) / 2u, top = (height - h) / 2u;
            bitmap::Header wheel = *header;
            wheel.width = static_cast<uint16_t>(width);
            wheel.height = static_cast<uint16_t>(height);
            wheel.rowBytes = static_cast<uint16_t>(width * bpp / 8u);
            milo::Bytes out;
            bitmap::write(out, wheel);
            out.insert(out.end(), colours, colours + palette);
            // Its darkest colours clear, as a digit's are, so the wheel's own
            // face shows behind it: the frame, drawn under every wheel.
            for (size_t c = out.size() - palette; c < out.size(); c += 4u)
                if (out[c] + out[c + 1u] + out[c + 2u] <= light(black) + 24u)
                    out[c + 3u] = 0u;
            const size_t start = out.size();
            out.resize(start + wheel.rowBytes * height, 0u);
            for (size_t y = 0; y < height; ++y)
                for (size_t x = 0; x < width; ++x)
                {
                    size_t k = black;
                    if (x >= left && x < left + w && y >= top && y < top + h)
                        k = at(std::min(x1 - 1u, x0 + size_t(float(x - left) / scale)),
                               std::min(y1 - 1u, y0 + size_t(float(y - top) / scale)));
                    uint8_t &b = out[start + y * wheel.rowBytes + x * bpp / 8u];
                    b = bpp == 8u ? static_cast<uint8_t>(k) : static_cast<uint8_t>(b | (k << (4u * (x & 1u))));
                }
            return out;
        }

        // The icon's wheel picture in a copy of the blank wheel's Tex. None
        // without the lighter's scene.
        std::optional<milo::Bytes> iconTex(const milo::Bytes &blank, uint32_t version)
        {
            const auto file = ark::readFile(0u, kLighterScene);
            const auto scene = file ? milo::read(*file) : std::nullopt;
            const auto at = scene ? milo::find(*scene, kLighter) : std::nullopt;
            const auto source = at ? milo::readTex(scene->bodies[*at], scene->version) : std::nullopt;
            const auto tex = milo::readTex(blank, version);
            const auto header = tex ? bitmap::read(blank.data() + tex->bitmap, blank.size() - tex->bitmap) : std::nullopt;
            if (!source || !header)
                return std::nullopt;
            const milo::Bytes &body = scene->bodies[*at];
            const auto picture = wheelPicture(body.data() + source->bitmap, body.size() - source->bitmap, header->width, header->height);
            if (!picture)
                return std::nullopt;
            milo::Bytes out(blank.begin(), blank.begin() + static_cast<std::ptrdiff_t>(tex->bitmap));
            const uint32_t bpp = (*picture)[1];
            std::memcpy(out.data() + tex->bpp, &bpp, 4u);
            out.insert(out.end(), picture->begin(), picture->end());
            return out;
        }

        // The score display's scene as the counter: four wheels, black with
        // white digits and lines, each with digit Mats of its own
        // (streak_w<wheel>_<digit>), the first the icon (streak_icon), the
        // rest of the score's six gone, and the shell's metal around it
        // (streak_frame_<n>).
        std::optional<ark::Made> makeCounterScene()
        {
            const auto file = ark::readFile(0u, kScore), shellFile = ark::readFile(0u, kShell);
            auto scene = file ? milo::read(*file) : std::nullopt;
            const auto shell = shellFile ? milo::read(*shellFile) : std::nullopt;
            if (!scene || !shell)
                return std::nullopt;
            const auto metal = milo::find(*shell, kMetal), metalMat = milo::find(*shell, kMetalMat);
            const auto frameMesh = milo::find(*scene, "score_num_frame.mesh");
            const auto blankTex = milo::find(*scene, "score_none.tex"), blankMat = milo::find(*scene, "score_none.mat");
            if (!metal || !metalMat || !frameMesh || !blankTex || !blankMat)
                return std::nullopt;

            std::vector<std::pair<std::string, milo::Bytes>> added;
            milo::Bytes icon = scene->bodies[*blankTex], iconMat = scene->bodies[*blankMat];
            if (auto picture = iconTex(icon, scene->version))
                icon = std::move(*picture);
            else
                invert(icon, scene->version);
            if (!replaceStr(iconMat, "score_none.tex", "streak_icon.tex"))
                return std::nullopt;
            whiten(iconMat);
            added.emplace_back("streak_icon.tex", std::move(icon));
            added.emplace_back("streak_icon.mat", std::move(iconMat));

            for (int n = 0; n <= 9; ++n)
            {
                const auto mat = milo::find(*scene, "score_" + std::to_string(n) + ".mat");
                if (!mat)
                    return std::nullopt;
                for (size_t w = 0; w < kWheels; ++w)
                {
                    milo::Bytes copy = scene->bodies[*mat];
                    whiten(copy);
                    added.emplace_back(wheelMat(w, n), std::move(copy));
                }
            }
            added.emplace_back(kMetal, shell->bodies[*metal]);
            added.emplace_back("streak_frame.mat", shell->bodies[*metalMat]);

            // The metal's pieces, from the frame quad as it will be trimmed.
            std::vector<milo::Bytes> pieces;
            {
                const milo::Bytes &quad = scene->bodies[*frameMesh];
                const auto at = quadVerts(quad);
                if (!at)
                    return std::nullopt;
                float local[12], edge, depth;
                std::memcpy(local, quad.data() + 17u, sizeof local);
                std::memcpy(&edge, quad.data() + *at + 4u, 4u);
                std::memcpy(&depth, quad.data() + *at + 4u + 8u, 4u);
                edge = std::fabs(edge), depth = std::fabs(depth);
                const float left = local[9] + local[0] * (-edge + 2.0f * edge * kFrameFrom), right = local[9] + local[0] * edge;
                const float bottom = local[11] - local[8] * depth, top = local[11] + local[8] * depth;
                const auto around = metalAround(left, right, bottom, top);
                for (size_t s = 0; s < around.size(); ++s)
                {
                    milo::Bytes piece = quad;
                    if (!replaceStr(piece, "score_num_frame.mat", "streak_frame.mat") ||
                        !replaceStr(piece, "score_num_frame.mesh", "streak_frame_" + std::to_string(s) + ".mesh"))
                        return std::nullopt;
                    const auto verts = quadVerts(piece);
                    if (!verts)
                        return std::nullopt;
                    placeQuad(piece, *verts, around[s]);
                    pieces.push_back(std::move(piece));
                }
            }

            for (size_t i = 0; i < scene->entries.size(); ++i)
            {
                const auto &[className, name] = scene->entries[i];
                milo::Bytes &body = scene->bodies[i];
                if (className == "Mat" && name.starts_with("score_") && name != "score_num_frame.mat")
                    whiten(body);
                else if (name == "score_num_frame.tex")
                {
                    invert(body, scene->version, frameKeeps);
                    closeEdges(body, scene->version);
                }
                else if (className == "Tex" && name.starts_with("score_"))
                    invert(body, scene->version);
                else if (name == "score_num_frame.mesh")
                    trimFrame(body);
                else if (className == "Group")
                {
                    for (const char *mesh : kDropped)
                        remove(body, mesh);
                    for (size_t s = pieces.size(); s-- > 0;)
                        addAfter(body, "score_num_frame.mesh", "streak_frame_" + std::to_string(s) + ".mesh");
                }
            }
            for (const char *mesh : kDropped)
                remove(scene->root, mesh);
            for (auto &[name, body] : added)
                milo::add(*scene, name.ends_with(".tex") ? "Tex" : "Mat", name, std::move(body));
            for (size_t s = 0; s < pieces.size(); ++s)
                milo::add(*scene, "Mesh", "streak_frame_" + std::to_string(s) + ".mesh", std::move(pieces[s]));
            return ark::held(milo::write(*scene));
        }
    }

    std::string wheelMat(size_t wheel, int digit)
    {
        return "streak_w" + std::to_string(wheel) + "_" + std::to_string(digit) + ".mat";
    }

    std::optional<ark::Made> makeScene() { return makeCounterScene(); }

    bool addCounter(milo::Dir &scene)
    {
        bool added = false;
        for (int n = 1; n <= 2; ++n)
        {
            const std::string score = "BandScoreDisplay" + std::to_string(n);
            const std::string shell = "BandStreakDisplay" + std::to_string(n);
            const std::string counter = kCounterDisplay + std::to_string(n);
            const auto from = milo::find(scene, score);
            if (!from || !milo::find(scene, shell) || milo::find(scene, counter))
                continue;
            // The Group that draws the shell, whose space script puts the
            // counter in (streak_counter.dta).
            std::optional<size_t> group;
            for (size_t i = 0; i < scene.entries.size() && !group; ++i)
                if (scene.entries[i].first == "Group" && listHolding(scene.bodies[i], shell))
                    group = i;
            milo::Bytes body = scene.bodies[*from];
            // Its own scene: the score's, or co-op's band score.
            if (!group || !(replaceStr(body, "../score_display.milo", "../streak_counter.milo") ||
                            replaceStr(body, "../coop_score.milo", "../streak_counter.milo")))
                continue;
            if (!addAfter(scene.bodies[*group], shell, counter))
                continue;
            milo::add(scene, "BandScoreDisplay", counter, std::move(body));
            added = true;
        }
        return added;
    }
}
