#include "hub/venue_posters.h"

#include "content/games.h"
#include "disc/ark.h"
#include "hub/picture.h"
#include "milo/milo.h"
#include "poster_faces.h"
#include "render/texture_store.h"

#define STBTT_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
#include <stb_truetype.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <numeric>
#include <thread>
#include <utility>
#include <vector>

// How a poster is made: made(game), once per game, on prepare's thread.
// mapPosters does GH2's and the 80s', gh1Posters GH1's; their steps are
// numbered alike. Every picture is a picture::Image, floats 0 to 255.
//
//   1. The map: the venue map's pages read off the disc and stacked into
//      one tall picture (GH2 and the 80s: stripOf, from venue select's
//      sv_map*.tex, its paint carried on past where its pages stop by
//      fieldOf, roadFoot and mirrored; GH1: its map1-3 bitmaps).
//   2. The paper strips: the map pages' own torn edges, cut off them
//      (Edges), and the 360 venue select's corner tape (tapes).
//   Then for each venue (kGh2, kGh80s, kGh1):
//   3. Its drawing (and on GH2 and the 80s its ribbon) scaled and laid
//      where the map has it.
//   4. The window: a poster-shaped crop of the map round the drawing, as
//      wide as makes the drawing fill the band between the title and the
//      name (kBand0 to kBand1, kRoom), slid to stay on the map's paint.
//   5. The drawing laid over the window, moved only to stay in the band.
//   6. finish: resized to kPaperW by kPaperH; text darkens the top and
//      bottom and letters TONIGHT!! (Rokk), LIVE AT and the name
//      (Helvetica); frame shrinks it inside the paper strips; tape puts
//      the tape over its top corners. Song defaults' "?" poster is blank
//      paper through the same finish, no map.
// made then turns each into RGBA bytes, which the hub's poster Tex is
// swapped for (hub/hub_scene.cpp).

namespace gh2::venue_posters
{
    namespace
    {
        using picture::Box;
        using picture::Image;
        using picture::Plane;
        using Rgb = std::array<float, 3>;

        // Map units the venue maps are laid out in, as pixels of a 512 page.
        constexpr float kPx = 2.56f;
        // The poster before its tape margin: 180 by 270 map units.
        constexpr int kPaperW = int(2 * 90 * kPx), kPaperH = int(270 * kPx);
        // The paper strips top and bottom the lettering keeps off.
        constexpr int kEdge = 16;
        // Where the venue's drawing goes, as fractions of the height: the
        // band between the title and the name.
        constexpr float kBand0 = 0.21f, kBand1 = 0.74f;
        // How much of the poster's width a drawing may take.
        constexpr float kRoom = 0.74f;
        // Map mirrored on past each end of GH2's and the 80s' map, px.
        constexpr int kMirror = 600;

        float median(std::vector<float> v)
        {
            if (v.empty())
                return 0.0f;
            const size_t mid = v.size() / 2;
            std::nth_element(v.begin(), v.begin() + mid, v.end());
            if (v.size() % 2)
                return v[mid];
            const float hi = v[mid];
            return (*std::max_element(v.begin(), v.begin() + mid) + hi) / 2.0f;
        }

        // numpy's percentile: linear between the two nearest ranks.
        float percentile(std::vector<float> v, float q)
        {
            if (v.empty())
                return 0.0f;
            std::sort(v.begin(), v.end());
            const float at = q / 100.0f * float(v.size() - 1);
            const size_t lo = size_t(at), hi = std::min(lo + 1, v.size() - 1);
            return v[lo] + (v[hi] - v[lo]) * (at - float(lo));
        }

        Rgb medianRgb(const std::vector<Rgb> &px)
        {
            Rgb out{};
            for (int c = 0; c < 3; ++c)
            {
                std::vector<float> v;
                v.reserve(px.size());
                for (const Rgb &p : px)
                    v.push_back(p[size_t(c)]);
                out[size_t(c)] = median(std::move(v));
            }
            return out;
        }

        float meanOf(const float *p)
        {
            return (p[0] + p[1] + p[2]) / 3.0f;
        }

        // A venue select scene's Tex pictures and Mesh placements.
        struct Scene
        {
            milo::Dir dir;
            bool paletted = true;

            const milo::Bytes *body(const std::string &cls, const std::string &name) const
            {
                return milo::body(dir, cls, name);
            }
            std::optional<Image> tex(const std::string &name) const
            {
                const milo::Bytes *b = body("Tex", name);
                const auto data = b ? textures::decodeTex(*b, dir.version, paletted) : nullptr;
                return data ? std::optional(picture::fromTexture(*data)) : std::nullopt;
            }
            // Its local transform: a 3 by 3, then its place (x at 9, z at 11).
            std::optional<std::array<float, 12>> xf(const std::string &name) const
            {
                const milo::Bytes *b = body("Mesh", name);
                if (!b || b->size() < 17u + 48u)
                    return std::nullopt;
                std::array<float, 12> out{};
                std::memcpy(out.data(), b->data() + 17, 48u);
                return out;
            }
            std::vector<std::string> named(const std::string &cls, const std::string &prefix) const
            {
                std::vector<std::string> out;
                for (const auto &[c, n] : dir.entries)
                    if (c == cls && n.starts_with(prefix))
                        out.push_back(n);
                std::sort(out.begin(), out.end());
                return out;
            }
        };

        std::optional<Scene> scene(size_t disc, const std::string &path, bool paletted = true)
        {
            const auto file = ark::readFile(disc, path);
            auto dir = file ? milo::read(*file) : std::nullopt;
            if (!dir)
                return std::nullopt;
            return Scene{std::move(*dir), paletted};
        }

        std::optional<Image> bitmapFile(size_t disc, const std::string &path)
        {
            const auto data = textures::readNow({disc, path, "", true});
            return data ? std::optional(picture::fromTexture(*data)) : std::nullopt;
        }

        Image page512(const Image &page)
        {
            return picture::resize(page, 512, 512);
        }

        // White drawn in `ink`, black kept: the 80s' venue select's tint.
        void inked(Image &image, const Rgb &ink)
        {
            for (size_t i = 0; i < image.px.size(); i += 4)
                for (int c = 0; c < 3; ++c)
                    image.px[i + c] *= ink[size_t(c)];
        }

        // ---- lettering -------------------------------------------------

        struct Face
        {
            stbtt_fontinfo info{};

            explicit Face(const unsigned char *data) { stbtt_InitFont(&info, data, stbtt_GetFontOffsetForIndex(data, 0)); }
        };

        const Face &rokk()
        {
            static const Face face(kPosterFaceRokk);
            return face;
        }

        const Face &helvetica()
        {
            static const Face face(kPosterFaceHelvetica);
            return face;
        }

        // A face at a size, as ImageFont.truetype(face, size): `size`
        // pixels an em.
        struct Font
        {
            const Face *face = nullptr;
            int size = 0;

            float scale() const { return stbtt_ScaleForMappingEmToPixels(&face->info, float(size)); }

            // How tall its H's ink stands.
            int caps() const
            {
                int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
                stbtt_GetCodepointBitmapBox(&face->info, 'H', scale(), scale(), &x0, &y0, &x1, &y1);
                return y1 - y0;
            }

            float length(const std::string &s) const
            {
                float out = 0.0f;
                for (const unsigned char ch : s)
                {
                    int advance = 0, lsb = 0;
                    stbtt_GetCodepointHMetrics(&face->info, ch, &advance, &lsb);
                    out += float(advance) * scale();
                }
                return out;
            }
        };

        // The biggest size whose caps stand `tall` or less and whose line
        // fits between the pads: traced faces draw small or large for their
        // size.
        Font fit(const Face &face, int tall, const std::string &s, int w, int pad)
        {
            int size = 8;
            while (true)
            {
                const Font f{&face, size + 2};
                if (f.caps() > tall || f.length(s) > float(w - 2 * pad))
                    return {&face, size};
                size += 2;
            }
        }

        // `s` centred on x `cx`, the top of its ink at `top` (as the Imaging
        // Library's "mt" anchor sets it), in `fill` over a keyline `stroke`
        // px wide in `line`.
        void letter(Image &p, int cx, int top, const std::string &s, const Font &font, const Rgb &fill,
                    const Rgb &line, int stroke = 4)
        {
            const float scale = font.scale();
            int inkTop = 0;
            for (const unsigned char ch : s)
            {
                int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
                stbtt_GetCodepointBitmapBox(&font.face->info, ch, scale, scale, &x0, &y0, &x1, &y1);
                if (x1 > x0)
                    inkTop = std::min(inkTop, y0);
            }
            const int baseline = top - inkTop;
            Plane cover(p.w, p.h);
            // The letters' box, grown by the keyline: nothing is drawn past it.
            int bx0 = p.w, by0 = p.h, bx1 = 0, by1 = 0;
            float pen = float(cx) - font.length(s) / 2.0f;
            for (const unsigned char ch : s)
            {
                const float shift = pen - std::floor(pen);
                int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
                stbtt_GetCodepointBitmapBoxSubpixel(&font.face->info, ch, scale, scale, shift, 0, &x0, &y0, &x1, &y1);
                const int gw = x1 - x0, gh = y1 - y0;
                if (gw > 0 && gh > 0)
                {
                    std::vector<unsigned char> glyph(size_t(gw) * gh);
                    stbtt_MakeCodepointBitmapSubpixel(&font.face->info, glyph.data(), gw, gh, gw, scale, scale, shift, 0,
                                                      ch);
                    const int ox = int(std::floor(pen)) + x0, oy = baseline + y0;
                    bx0 = std::min(bx0, ox), by0 = std::min(by0, oy);
                    bx1 = std::max(bx1, ox + gw), by1 = std::max(by1, oy + gh);
                    for (int y = 0; y < gh; ++y)
                        for (int x = 0; x < gw; ++x)
                            if (ox + x >= 0 && oy + y >= 0 && ox + x < p.w && oy + y < p.h)
                                cover.at(ox + x, oy + y) =
                                    std::max(cover.at(ox + x, oy + y), glyph[size_t(y) * gw + x] / 255.0f);
                }
                int advance = 0, lsb = 0;
                stbtt_GetCodepointHMetrics(&font.face->info, ch, &advance, &lsb);
                pen += float(advance) * scale;
            }
            // The keyline: the letters grown by `stroke`, round.
            Plane ring(p.w, p.h);
            bx0 = std::max(0, bx0 - stroke - 1), by0 = std::max(0, by0 - stroke - 1);
            bx1 = std::min(p.w, bx1 + stroke + 1), by1 = std::min(p.h, by1 + stroke + 1);
            for (int y = by0; y < by1; ++y)
                for (int x = bx0; x < bx1; ++x)
                {
                    float m = 0.0f;
                    for (int j = -stroke - 1; j <= stroke + 1; ++j)
                        for (int i = -stroke - 1; i <= stroke + 1; ++i)
                        {
                            const int qx = x + i, qy = y + j;
                            if (qx < 0 || qy < 0 || qx >= p.w || qy >= p.h)
                                continue;
                            const float c = cover.at(qx, qy);
                            if (c <= 0.0f)
                                continue;
                            const float reach = std::clamp(float(stroke) + 0.5f - std::hypot(float(i), float(j)), 0.0f, 1.0f);
                            m = std::max(m, c * reach);
                        }
                    ring.at(x, y) = m;
                }
            for (int y = by0; y < by1; ++y)
                for (int x = bx0; x < bx1; ++x)
                {
                    float *d = p.at(x, y);
                    for (const auto &[a, rgb] : {std::pair{ring.at(x, y), line}, std::pair{cover.at(x, y), fill}})
                    {
                        if (a <= 0.0f)
                            continue;
                        for (int c = 0; c < 3; ++c)
                            d[c] = rgb[size_t(c)] * a + d[c] * (1.0f - a);
                        d[3] = std::max(d[3], a * 255.0f);
                    }
                }
        }

        constexpr Rgb kCream = {242, 232, 204};
        struct Colours
        {
            Rgb title, name, line;
        };
        Colours coloursOf(const std::string &game)
        {
            if (game == "gh80s")
                return {kCream, {255, 92, 128}, {30, 6, 20}};
            if (game == "gh1")
                return {kCream, {240, 120, 50}, {30, 18, 12}};
            return {kCream, {120, 196, 255}, {10, 18, 34}};
        }

        // TONIGHT!! over the map, LIVE AT and the venue's name under it, on
        // a soft dark fade behind each so they read over a busy map.
        void text(Image &p, const std::string &title, const std::string &game)
        {
            for (int y = 0; y < p.h; ++y)
            {
                const float r = float(y), H = float(p.h);
                const float shade = std::max(std::clamp(1.0f - r / (H * 0.17f), 0.0f, 1.0f),
                                             std::clamp((r - H * 0.70f) / (H * 0.25f), 0.0f, 1.0f)) * 0.55f;
                for (int x = 0; x < p.w; ++x)
                    for (int c = 0; c < 3; ++c)
                        p.at(x, y)[c] *= 1.0f - shade;
            }
            const Colours col = coloursOf(game);
            const int w = p.w, pad = int(float(w) * 0.12f);
            const std::string tonight = "TONIGHT!!", live = "LIVE AT";
            letter(p, w / 2, kEdge + 10, tonight, fit(rokk(), 74, tonight, w, pad), col.title, col.line);
            // A name that would shrink well below full size on one line goes
            // on two, the last word alone on the second (TOXIC SUMMER /
            // TOUR), a little smaller.
            const Font one = fit(helvetica(), 44, title, w, pad);
            std::vector<std::string> lines{title};
            const size_t lastSpace = title.rfind(' ');
            if (one.caps() < 36 && lastSpace != std::string::npos)
                lines = {title.substr(0, lastSpace), title.substr(lastSpace + 1)};
            const Font liveFont = fit(helvetica(), 28, live, w, pad);
            if (lines.size() == 1)
            {
                letter(p, w / 2, p.h - kEdge - 118, live, liveFont, col.title, col.line);
                letter(p, w / 2, p.h - kEdge - 76, title, one, col.name, col.line);
                return;
            }
            Font font = fit(helvetica(), 38, lines[0], w, pad);
            for (const std::string &s : lines)
                if (const Font f = fit(helvetica(), 38, s, w, pad); f.caps() < font.caps())
                    font = f;
            const int step = font.caps() + 12, y0 = p.h - kEdge - 76 - step;
            letter(p, w / 2, y0 - 42, live, liveFont, col.title, col.line);
            for (size_t i = 0; i < lines.size(); ++i)
                letter(p, w / 2, y0 + int(i) * step, lines[i], font, col.name, col.line);
        }

        // ---- paper edge and tape ---------------------------------------

        // A game's map's own paper: strips cut from its top, bottom and a
        // middle page at 512 (the 80s' top page is short: its bottom's,
        // flipped).
        struct Edges
        {
            Image top, bottom, left, right;
        };

        void frame(Image &p, const Edges &e)
        {
            const int w = p.w, h = p.h;
            const float s = float(w) / 512.0f;
            const int th = int(float(e.top.h) * s), bh = int(float(e.bottom.h) * s);
            const int lw = int(float(e.left.w) * s), rw = int(float(e.right.w) * s);
            Image out(w, h);
            picture::paste(out, picture::resize(p, w - lw - rw, h - th - bh, picture::Filter::Lanczos), lw, th);
            picture::over(out, picture::resize(e.left, lw, h - th - bh), 0, th);
            picture::over(out, picture::resize(e.right, rw, h - th - bh), w - rw, th);
            picture::over(out, picture::resize(e.top, w, th), 0, 0);
            picture::over(out, picture::resize(e.bottom, w, bh), 0, h - bh);
            p = std::move(out);
        }

        constexpr int kMargin = 40;

        // The poster on a clear margin, two strips of venue select's corner
        // tape across its top corners, half on it and half off, and the
        // whole brought back to the poster's size.
        void tape(Image &p, const std::pair<Image, Image> &strips)
        {
            Image on(p.w + 2 * kMargin, p.h + 2 * kMargin);
            picture::over(on, p, kMargin, kMargin);
            for (const auto &[t, angle, right] :
                 {std::tuple{&strips.first, 40.0f, false}, std::tuple{&strips.second, -40.0f, true}})
            {
                const Box b = picture::bbox(*t);
                const Image s =
                    picture::rotate(picture::resize(picture::crop(*t, b.x0, b.y0, b.x1, b.y1), 200, 92), angle);
                // Its middle a little in from the corner: more of it on the
                // poster.
                const int cx = kMargin + (right ? p.w - 14 : 14);
                const int ax = cx - s.w / 2, ay = kMargin + 14 - s.h / 2;
                Image shadow(s.w, s.h);
                for (size_t i = 0; i < s.px.size(); i += 4)
                    shadow.px[i + 3] = s.px[i + 3] * 0.35f;
                picture::blur(shadow, 4.0f);
                picture::over(on, shadow, ax + 3, ay + 4);
                picture::over(on, s, ax, ay);
            }
            p = picture::resize(on, p.w, p.h);
        }

        // ---- GH2's and the 80s' map ------------------------------------

        // `src` shifted as far as takes its ground colour (its commonest) to
        // `like`'s painted one, its own shading kept.
        Rgb commonest(const std::vector<Rgb> &px)
        {
            std::map<int, std::pair<int, Rgb>> buckets;
            for (const Rgb &p : px)
            {
                const int key = int(p[0] / 6) * 10000 + int(p[1] / 6) * 100 + int(p[2] / 6);
                auto &[n, sum] = buckets[key];
                ++n;
                for (int c = 0; c < 3; ++c)
                    sum[size_t(c)] += p[size_t(c)];
            }
            const auto most = std::max_element(buckets.begin(), buckets.end(),
                                               [](const auto &a, const auto &b) { return a.second.first < b.second.first; });
            Rgb out = most->second.second;
            for (float &v : out)
                v /= float(most->second.first);
            return out;
        }

        Image matchColour(const Image &src, const Image &like)
        {
            std::vector<Rgb> s, l;
            for (size_t i = 0; i < src.px.size(); i += 4)
                s.push_back({std::floor(src.px[i]), std::floor(src.px[i + 1]), std::floor(src.px[i + 2])});
            for (size_t i = 0; i < like.px.size(); i += 4)
                if (like.px[i + 3] > 200.0f)
                    l.push_back({std::floor(like.px[i]), std::floor(like.px[i + 1]), std::floor(like.px[i + 2])});
            const Rgb ms = commonest(s), ml = l.empty() ? ms : commonest(l);
            Image out = src;
            for (size_t i = 0; i < out.px.size(); i += 4)
                for (int c = 0; c < 3; ++c)
                    out.px[i + c] = std::clamp(out.px[i + c] - ms[size_t(c)] + ml[size_t(c)], 0.0f, 255.0f);
            return out;
        }

        // GH2's grassland under Stonehenge (its top page, x 66 to 262, rows
        // 262 to 360: grass tufts, left of the road), its stone cloned out,
        // mirrored across, its broad shading flattened.
        std::optional<Image> fieldOf(const Scene &gh2)
        {
            const auto top = gh2.tex("sv_map6.tex");
            if (!top)
                return std::nullopt;
            Image half = picture::crop(page512(*top), 66, 262, 262, 360);
            // Its stone cloned out with the bare ground beside it, same rows.
            {
                const Plane hl = picture::mean(half);
                const float med = median(hl.v);
                int x0 = half.w, y0 = half.h, x1 = -1, y1 = -1;
                for (int y = 40; y < 80; ++y)
                    for (int x = 140; x < 196; ++x) // round the stone (x 149 to 183, rows 60 to 69 at its darkest)
                        if (std::fabs(hl.at(x, y) - med) > 8.0f)
                            x0 = std::min(x0, x), x1 = std::max(x1, x), y0 = std::min(y0, y), y1 = std::max(y1, y);
                if (x1 >= 0)
                {
                    x0 -= 4, x1 += 5, y0 -= 4, y1 += 5;
                    const Image patch = picture::crop(half, x0 - (x1 - x0) - 6, y0, x0 - 6, y1);
                    Plane feather(patch.w, patch.h);
                    for (int y = 2; y <= patch.h - 3; ++y)
                        for (int x = 2; x <= patch.w - 3; ++x)
                            feather.at(x, y) = 1.0f;
                    picture::blur(feather, 1.5f);
                    picture::paste(half, patch, x0, y0, feather);
                }
            }
            Image row(512, half.h);
            for (int x = 0, i = 0; x < 512; x += half.w, ++i)
                picture::paste(row, i % 2 ? picture::flipLeftRight(half) : half, x, 0);
            // Its broad shading flattened (high pass): the page's soft dark
            // patch by the road, mirrored into the middle, gone.
            Image low = picture::medianFilter(row, 31);
            picture::blur(low, 14.0f);
            Rgb lowMean{};
            for (size_t i = 0; i < low.px.size(); i += 4)
                for (int c = 0; c < 3; ++c)
                    lowMean[size_t(c)] += low.px[i + c];
            for (float &v : lowMean)
                v /= float(low.px.size() / 4);
            for (size_t i = 0; i < row.px.size(); i += 4)
                for (int c = 0; c < 3; ++c)
                    row.px[i + c] = std::clamp(row.px[i + c] - low.px[i + c] + lowMean[size_t(c)], 0.0f, 255.0f);
            // And the pale stipple that outlined that patch, a few shades
            // over the ground, recoloured to it; the tufts are far paler.
            const Plane lum = picture::mean(row);
            const float g = median(lum.v);
            Plane tufts(row.w, row.h);
            for (size_t i = 0; i < lum.v.size(); ++i)
                tufts.v[i] = lum.v[i] > g + 15.0f ? 1.0f : 0.0f;
            tufts = picture::maxFilter(tufts, 5);
            for (size_t i = 0; i < lum.v.size(); ++i)
                if (lum.v[i] > g + 2.0f && lum.v[i] < g + 15.0f && tufts.v[i] == 0.0f)
                    for (int c = 0; c < 3; ++c)
                        row.px[i * 4 + c] += g - lum.v[i];
            for (float &v : row.px)
                v = std::floor(v);
            return row;
        }

        // roadFoot's pieces, in the order it lays them.
        using Point = std::array<float, 2>;

        // The U-bend below the cut (row `cut`): its outer edge from where the
        // cut leaves it round under the bend and up to the road's end at the
        // corn, how far along that edge each point is, and the edge closed
        // back along the cut.
        struct Bend
        {
            std::vector<Point> edge;
            std::vector<float> along;
            std::vector<Point> shape;
        };

        Bend bendBelow(int cut)
        {
            using P = Point;
            const auto bez = [](P p0, P p1, P p2, P p3, int n)
            {
                std::vector<P> out;
                for (int i = 0; i < n; ++i)
                {
                    const float t = float(i) / float(n - 1), u = 1 - t;
                    out.push_back({u * u * u * p0[0] + 3 * u * u * t * p1[0] + 3 * u * t * t * p2[0] + t * t * t * p3[0],
                                   u * u * u * p0[1] + 3 * u * u * t * p1[1] + 3 * u * t * t * p2[1] + t * t * t * p3[1]});
                }
                return out;
            };
            // Both edges run on as the page has them: the outer from x 73 at
            // row 460 at 0.375 a row, the end from x 288 at row 460 at -1.5
            // a row, down to where it meets the lower edge (x 269).
            const P t0{0.351f, 0.936f}, t3{0.887f, -0.462f};
            const P p0{73 + 0.375f * float(cut - 460), float(cut)}, p3{215, 501}, corner{269, 472.7f};
            const float endX = 288 - 1.5f * float(cut - 460);
            std::vector<P> edge = bez(p0, {p0[0] + 50 * t0[0], p0[1] + 50 * t0[1]},
                                      {p3[0] - 40 * t3[0], p3[1] - 40 * t3[1]}, p3, 400);
            for (int i = 1; i < 200; ++i)
            {
                const float t = float(i) / 199.0f;
                edge.push_back({p3[0] + (corner[0] - p3[0]) * t, p3[1] + (corner[1] - p3[1]) * t});
            }
            std::vector<float> along(edge.size(), 0.0f);
            for (size_t i = 1; i < edge.size(); ++i)
                along[i] = along[i - 1] + std::hypot(edge[i][0] - edge[i - 1][0], edge[i][1] - edge[i - 1][1]);
            std::vector<P> shape = edge;
            shape.push_back({endX, float(cut)});
            shape.push_back({p0[0], float(cut)});
            return {edge, along, shape};
        }

        // Inside the bend, each pixel the page's own road at the same depth
        // in from the edge (its straight stretch at the top, x 330 to 420,
        // ping-ponged along), in the road's shade where it meets the cut.
        void fillBend(Image &layer, const Image &a, int cut, const Bend &bend)
        {
            const std::vector<Point> &edge = bend.edge, &shape = bend.shape;
            const std::vector<float> &along = bend.along;
            const auto inside = [&](float x, float y)
            {
                bool in = false;
                for (size_t i = 0, j = shape.size() - 1; i < shape.size(); j = i++)
                    if ((shape[i][1] > y) != (shape[j][1] > y) &&
                        x < (shape[j][0] - shape[i][0]) * (y - shape[i][1]) / (shape[j][1] - shape[i][1]) + shape[i][0])
                        in = !in;
                return in;
            };
            const int h = layer.h;
            const float lengthOf = 90.0f * std::hypot(1.0f, 0.355f);
            const float c = 1.0f / std::hypot(1.0f, 0.355f), s = 0.355f / std::hypot(1.0f, 0.355f);
            // In this road's own shade where it meets the cut.
            std::vector<Rgb> srcPx, nearPx;
            for (int y = 80; y < 90; ++y)
                for (int x = 360; x < 380; ++x)
                    srcPx.push_back({a.at(x, y)[0], a.at(x, y)[1], a.at(x, y)[2]});
            const Rgb srcRoad = medianRgb(srcPx);
            const float srcMean = (srcRoad[0] + srcRoad[1] + srcRoad[2]) / 3.0f;
            for (int y = cut - 8; y < cut; ++y)
                for (int x = 100; x < 280; ++x)
                    if (std::fabs(meanOf(a.at(x, y)) - srcMean) < 8.0f) // its paint, not lines or ground
                        nearPx.push_back({a.at(x, y)[0], a.at(x, y)[1], a.at(x, y)[2]});
            const Rgb nearRoad = nearPx.empty() ? srcRoad : medianRgb(nearPx);
            for (int y = cut; y < cut + 70 && y < h; ++y)
                for (int x = 51; x < 300; ++x)
                {
                    float best = 1e18f;
                    size_t k = 0;
                    for (size_t i = 0; i < edge.size(); ++i)
                    {
                        const float dx = float(x) - edge[i][0], dy = float(y) - edge[i][1], d = dx * dx + dy * dy;
                        if (d < best)
                            best = d, k = i;
                    }
                    float u = std::fmod(along[k], 2.0f * lengthOf);
                    if (u > lengthOf)
                        u = 2.0f * lengthOf - u;
                    const float in = std::min(std::sqrt(best), 44.0f) - 0.5f;
                    float px[4];
                    picture::sample(a, 330 + u * c + in * s, 94 + u * s - in * c, px);
                    float cover = 0.0f;
                    for (int j = 0; j < 4; ++j)
                        for (int i = 0; i < 4; ++i)
                            cover += inside(float(x) + (float(i) + 0.5f) / 4.0f, float(y) + (float(j) + 0.5f) / 4.0f);
                    float *o = layer.at(x, y);
                    for (int ch = 0; ch < 3; ++ch)
                        o[ch] = px[ch] + nearRoad[size_t(ch)] - srcRoad[size_t(ch)];
                    o[3] = cover / 16.0f * 255.0f;
                }
        }

        // The shaded strip along the outer edge above the cut, which the
        // bend's fill doesn't carry: the road's own colour.
        void shadeOuterStrip(Image &layer, const Image &a, int cut)
        {
            std::vector<Rgb> roadPx;
            for (int y = 440; y < 460; ++y)
                for (int x = 230; x < 260; ++x)
                    roadPx.push_back({a.at(x, y)[0], a.at(x, y)[1], a.at(x, y)[2]});
            const Rgb road = medianRgb(roadPx);
            const float rl = (road[0] + road[1] + road[2]) / 3.0f;
            for (int y = 420; y < cut; ++y)
                for (int x = 56; x < 105; ++x)
                    if (const float l = meanOf(a.at(x, y)); l > rl - 14 && l < rl - 2)
                        std::copy_n(std::array{road[0], road[1], road[2], 255.0f}.data(), 4, layer.at(x, y));
        }

        // The middle post (x 190, crossbars rows 442 to 462), cut at the
        // page's edge: the right post, crossbars and pole (x 307 to 333 from
        // row 408, only its pole's x 318 to 328 past the corn at row 438),
        // laid over it 130 left and 24 down, crossbars on crossbars, in the
        // middle post's paler ink, where it is dark against the ground behind.
        void copyMiddlePost(Image &layer, const Image &a, int cut)
        {
            std::vector<float> bgL;
            for (int y = 440; y < 464; ++y)
                for (int x = 306; x < 316; ++x)
                    bgL.push_back(meanOf(a.at(x, y)));
            const float bg = median(bgL);
            const int rowsHigh = cut - 408;
            float darkest = 1e9f;
            for (int y = 0; y < rowsHigh; ++y)
                for (int x = 0; x < 26; ++x)
                    darkest = std::min(darkest, meanOf(a.at(307 + x, 408 + y)));
            Plane alpha(26, rowsHigh);
            std::vector<Rgb> srcInk;
            for (int y = 0; y < rowsHigh; ++y)
                for (int x = 0; x < 26; ++x)
                {
                    const bool post = 408 + y < 438 || (x >= 11 && x < 21);
                    alpha.at(x, y) = post ? std::clamp((bg - meanOf(a.at(307 + x, 408 + y))) / (bg - darkest), 0.0f, 1.0f) : 0.0f;
                    if (alpha.at(x, y) > 0.9f)
                        srcInk.push_back({a.at(307 + x, 408 + y)[0], a.at(307 + x, 408 + y)[1], a.at(307 + x, 408 + y)[2]});
                }
            std::vector<Rgb> mid;
            std::vector<float> midL;
            for (int y = 452; y < cut; ++y)
                for (int x = 192; x < 197; ++x)
                    mid.push_back({a.at(x, y)[0], a.at(x, y)[1], a.at(x, y)[2]}), midL.push_back(meanOf(a.at(x, y)));
            const float midCut = percentile(midL, 30);
            std::vector<Rgb> midDark;
            for (size_t i = 0; i < mid.size(); ++i)
                if (midL[i] < midCut)
                    midDark.push_back(mid[i]);
            const Rgb midInk = medianRgb(midDark), rightInk = medianRgb(srcInk);
            const int n = cut - 432;
            for (int y = 0; y < n; ++y)
                for (int x = 0; x < 26; ++x)
                {
                    const float al = alpha.at(x, y);
                    float *o = layer.at(177 + x, 432 + y);
                    const float *under = a.at(177 + x, 432 + y), *src = a.at(307 + x, 408 + y);
                    for (int ch = 0; ch < 3; ++ch)
                        o[ch] = under[ch] * (1 - al) + (src[ch] + midInk[size_t(ch)] - rightInk[size_t(ch)]) * al;
                    if (al > 0.0f)
                        o[3] = 255.0f;
                }
        }

        // The middle post's pole below the cut: its own cross-section from
        // its last rows above it (each column's palest of the last six: the
        // pole, not the lines across it), carried on down at the pole's own
        // lean (3.5 px over its 24 rows above), past the bend's edge into
        // the grass.
        void poleBelowCut(Image &layer, const Image &a, int cut, const std::vector<Point> &edge)
        {
            float footY = 0.0f;
            for (const Point &e : edge)
                if (std::fabs(e[0] - 194.0f) < 2.0f)
                    footY = std::max(footY, e[1]);
            const int foot = int(footY) + 10, x0 = 184;
            std::array<Rgb, 16> col{};
            std::array<float, 16> cl{};
            for (int x = 0; x < 16; ++x)
            {
                int pick = cut - 6;
                for (int y = cut - 6; y < cut; ++y)
                    if (meanOf(layer.at(x0 + x, y)) > meanOf(layer.at(x0 + x, pick)))
                        pick = y;
                col[size_t(x)] = {layer.at(x0 + x, pick)[0], layer.at(x0 + x, pick)[1], layer.at(x0 + x, pick)[2]};
                cl[size_t(x)] = (col[size_t(x)][0] + col[size_t(x)][1] + col[size_t(x)][2]) / 3.0f;
            }
            std::vector<float> roadL;
            for (int y = cut - 12; y < cut; ++y)
                for (int x = 200; x < 230; ++x)
                    roadL.push_back(meanOf(a.at(x, y)));
            const float roadLum = median(roadL), clMin = *std::min_element(cl.begin(), cl.end());
            std::array<float, 16> prof{};
            for (int x = 0; x < 16; ++x)
            {
                prof[size_t(x)] = std::clamp((roadLum - cl[size_t(x)]) / (roadLum - clMin), 0.0f, 1.0f);
                if (prof[size_t(x)] < 0.2f)
                    prof[size_t(x)] = 0.0f;
            }
            // The stem alone: the run round its darkest column, not a
            // crossbar's end.
            const int cMax = int(std::max_element(prof.begin(), prof.end()) - prof.begin());
            int lo = cMax, hi = cMax;
            while (lo > 0 && prof[size_t(lo - 1)] > 0.0f)
                --lo;
            while (hi < 15 && prof[size_t(hi + 1)] > 0.0f)
                ++hi;
            for (int x = 0; x < 16; ++x)
                if (x < lo || x > hi)
                    prof[size_t(x)] = 0.0f;
            std::vector<Rgb> inkPx;
            for (int y = cut - 3; y < cut; ++y)
                for (int x = lo; x <= hi; ++x)
                    if (meanOf(layer.at(x0 + x, y)) < cl[size_t(cMax)] + 4.0f)
                        inkPx.push_back({layer.at(x0 + x, y)[0], layer.at(x0 + x, y)[1], layer.at(x0 + x, y)[2]});
            const Rgb ink = inkPx.empty() ? col[size_t(cMax)] : medianRgb(inkPx);
            const int rows = foot - cut;
            for (int r = 0; r < rows; ++r)
            {
                const float taper = r >= rows - 4 ? std::array{0.8f, 0.55f, 0.3f, 0.1f}[size_t(r - (rows - 4))] : 1.0f;
                for (int x = 0; x < 16; ++x)
                {
                    const float q = float(x) - 3.5f / 24.0f * float(r + 1);
                    float pa = 0.0f;
                    if (q >= 0.0f && q <= 15.0f)
                    {
                        const int i = std::min(int(q), 14);
                        pa = prof[size_t(i)] + (prof[size_t(i + 1)] - prof[size_t(i)]) * (q - float(i));
                    }
                    pa *= taper;
                    float *o = layer.at(x0 + x, cut + r);
                    const float la = o[3] / 255.0f, oa = pa + la * (1 - pa);
                    for (int ch = 0; ch < 3; ++ch)
                        o[ch] = (ink[size_t(ch)] * pa + o[ch] * la * (1 - pa)) / std::max(oa, 1e-6f);
                    o[3] = oa * 255.0f;
                }
            }
        }

        // The bottom page's road, its U-bend finished below the page's cut
        // (row `cut`), on a layer in the page's place.
        Image roadFoot(const Image &a, int cut)
        {
            const Bend bend = bendBelow(cut);
            Image layer(512, 620);
            fillBend(layer, a, cut, bend);
            shadeOuterStrip(layer, a, cut);
            copyMiddlePost(layer, a, cut);
            poleBelowCut(layer, a, cut, bend.edge);
            return layer;
        }

        bool paintRow(const Image &a, int y, float most)
        {
            int n = 0;
            for (int x = 100; x < 412; ++x)
            {
                const float *p = a.at(x, y);
                n += std::min({p[0], p[1], p[2]}) < most && p[3] > 200.0f;
            }
            return float(n) / 312.0f > 0.6f;
        }

        // The map with its paint mirrored on past its top, where its paper
        // starts, and below its bottom, under Nilbog High, open field faded
        // in over the page's last rows mirrored down and the road's U-bend
        // closed. `top` moves up by kMirror px.
        Image mirrored(const Image &strip, const Image &field)
        {
            int first = -1, last = -1;
            for (int y = 0; y < strip.h; ++y)
                if (paintRow(strip, y, 190.0f))
                    last = y, first = first < 0 ? y : first;
            first += 6, last -= 6;
            const Image body = picture::crop(strip, 0, first, strip.w, last + 1);
            Image out(strip.w, strip.h + 2 * kMirror);
            picture::paste(out, strip, 0, kMirror);
            picture::paste(out, body, 0, kMirror + first);
            const Image up =
                picture::flipTopBottom(picture::crop(body, 0, 0, body.w, std::min(kMirror + first, body.h)));
            picture::paste(out, up, 0, kMirror + first - up.h);
            // The page is left as drawn down to its last whole row of paint
            // (its last row is a shade pale, half paper: dropped too).
            int cut = last;
            for (int y = last; y < strip.h; ++y)
            {
                float m = 0.0f;
                for (int x = 100; x < 412; ++x)
                    m += std::floor(std::min({strip.at(x, y)[0], strip.at(x, y)[1], strip.at(x, y)[2]}));
                if (m / 312.0f > 120.0f)
                {
                    cut = y;
                    break;
                }
            }
            cut -= 1;
            const Image land = matchColour(field, picture::crop(body, 0, body.h - 80, body.w, body.h));
            Image ground(out.w, out.h - kMirror - cut);
            for (int y = 0, i = 0; y < ground.h; y += land.h, ++i)
                picture::paste(ground, i % 2 ? picture::flipTopBottom(land) : land, 0, y);
            constexpr int kBlend = 16;
            const Image under = picture::flipTopBottom(picture::crop(strip, 0, cut - kBlend, out.w, cut));
            Plane ramp(1, 256);
            for (int y = 0; y < 256; ++y)
                ramp.at(0, y) = float(y);
            ramp = picture::resize(ramp, 1, kBlend);
            Plane fade(out.w, kBlend);
            Plane ul = picture::luma(under);
            for (float &v : ul.v)
                v = std::floor(v);
            const float bright = percentile(ul.v, 20) + 12.0f;
            Plane road(out.w, kBlend);
            for (size_t i = 0; i < ul.v.size(); ++i)
                road.v[i] = ul.v[i] > bright ? 1.0f : 0.0f;
            road = picture::maxFilter(road, 5);
            for (int y = 0; y < kBlend; ++y)
                for (int x = 0; x < out.w; ++x)
                    // The road's pale paint not mirrored: past its end it
                    // would read as more road.
                    fade.at(x, y) = road.at(x, y) > 0.0f ? 1.0f : std::clamp(ramp.at(0, y), 0.0f, 255.0f) / 255.0f;
            picture::paste(out, ground, 0, kMirror + cut);
            Image band = under;
            picture::paste(band, picture::crop(ground, 0, 0, out.w, kBlend), 0, 0, fade);
            picture::paste(out, band, 0, kMirror + cut);
            const int page = strip.h - 512;
            picture::over(out, roadFoot(picture::crop(strip, 0, page, 512, strip.h), cut - page), 0, kMirror + page);
            return out;
        }

        // The map's pages stacked as the scene stacks them, and where its
        // top is in map units (past the mirrored top).
        std::optional<std::pair<Image, float>> stripOf(const Scene &sc, const Image &field)
        {
            const auto pages = sc.named("Tex", "sv_map");
            if (pages.empty())
                return std::nullopt;
            std::vector<float> zs;
            for (const std::string &pg : pages)
            {
                const auto m = sc.xf(pg.substr(0, pg.size() - 4) + ".mesh");
                if (!m)
                    return std::nullopt;
                zs.push_back((*m)[11]);
            }
            const float top = *std::max_element(zs.begin(), zs.end()) + 100.0f;
            Image strip(512, int((top - *std::min_element(zs.begin(), zs.end()) + 100.0f) * kPx));
            for (size_t i = 0; i < pages.size(); ++i)
            {
                const auto t = sc.tex(pages[i]);
                if (!t)
                    return std::nullopt;
                picture::paste(strip, page512(*t), 0, int((top - zs[i] - 100.0f) * kPx));
            }
            return std::pair{mirrored(strip, field), top + float(kMirror) / kPx};
        }

        // How much of each box of a picture is paper or empty, from the
        // running sums of that.
        struct Unpainted
        {
            int w = 0, h = 0;
            std::vector<double> sum; // (w + 1) * (h + 1)

            explicit Unpainted(const Image &img) : w(img.w), h(img.h), sum(size_t(img.w + 1) * (img.h + 1), 0.0)
            {
                for (int y = 0; y < h; ++y)
                    for (int x = 0; x < w; ++x)
                    {
                        const float *p = img.at(x, y);
                        const double v = std::min({p[0], p[1], p[2]}) > 200.0f || p[3] < 200.0f;
                        sum[size_t(y + 1) * (w + 1) + x + 1] = v + sum[size_t(y) * (w + 1) + x + 1] +
                                                                sum[size_t(y + 1) * (w + 1) + x] - sum[size_t(y) * (w + 1) + x];
                    }
            }
            bool painted(int y0, int y1, int x0, int x1) const
            {
                x0 = std::clamp(x0, 0, w), x1 = std::clamp(x1, 0, w), y0 = std::clamp(y0, 0, h), y1 = std::clamp(y1, 0, h);
                const double area = double(x1 - x0) * (y1 - y0);
                if (area <= 0)
                    return false;
                const auto at = [&](int x, int y) { return sum[size_t(y) * (w + 1) + x]; };
                return (at(x1, y1) - at(x0, y1) - at(x1, y0) + at(x0, y0)) / area < 0.01;
            }
        };

        std::pair<Image, Image> tapes()
        {
            if (const auto disc = games::disc("gh2x"))
                if (const auto sc = scene(*disc, "ui/gen/sel_venue.milo_xbox", false))
                {
                    const auto l = sc->tex("tape_diag_L1.tex"), r = sc->tex("tape_diag_R1.tex");
                    if (l && r)
                        return {*l, *r};
                }
            // No 360 disc: GH2's own masking tape, turned.
            if (const auto disc = games::disc("gh2"))
                if (const auto sc = scene(*disc, "ui/gen/sel_venue.milo_ps2"))
                    if (const auto t = sc->tex("tape4.tex"))
                        return {*t, picture::flipLeftRight(*t)};
            return {Image(), Image()};
        }

        // Each venue as its own world names it: its locale's name where that
        // is a place, its address or the name on its drawing where the
        // locale gives the event or a stand-in.
        const std::vector<Venue> kGh2 = {
            {"battle", "battle", "NILBOG HIGH"},       {"small1", "small1", "THE RAT CELLAR"},
            {"small2", "small2", "THE BLACKOUT BAR"},  {"big", "bigclub", "REDOCTANE CLUB"},
            {"theatre", "theater", "ROCK CITY THEATER"}, {"fest", "fest", "VANS WARPED TOUR"},
            {"arena", "arena", "HARMONIX ARENA"},       {"stone", "stone", "STONEHENGE"},
        };
        const std::vector<Venue> kGh80s = {
            {"battle", "battle", "NILBOG HIGH"},         {"small1", "small1", "THE RAT CELLAR"},
            {"small2", "small2", "THE BLACKOUT BAR"},    {"theatre", "theater", "ROCK CITY THEATER"},
            {"fest", "fest", "ROCK FOR SAFETY TOUR"},     {"arena", "arena", "HARMONIX ARENA"},
        };
        const std::vector<Venue> kGh1 = {
            {"battle", "basement", "34 WINSHIP ST"},   {"small1", "smallclub", "FREAK PIT"},
            {"big", "bigclub", "RED OCTANE"},          {"theatre", "theater", "REPUBLIK THEATER"},
            {"fest", "fest", "TOXIC SUMMER TOUR"},     {"arena", "arena", "THE GARDEN"},
        };

        // A poster for no venue in particular (song defaults: each song its
        // own): blank paper in its map's own paper colour (the middle of its
        // edges' opaque near-white pixels), in its paper edge, and a big ?
        // on it, nothing else.
        constexpr const char *kUnknown = "unknown";

        Image blank(const Edges &edges, int w, int h)
        {
            std::vector<Rgb> paper;
            for (const Image *e : {&edges.top, &edges.bottom, &edges.left, &edges.right})
                for (size_t i = 0; i < e->px.size(); i += 4)
                    if (e->px[i + 3] > 250.0f && std::min({e->px[i], e->px[i + 1], e->px[i + 2]}) > 190.0f)
                        paper.push_back({e->px[i], e->px[i + 1], e->px[i + 2]});
            const Rgb c = paper.empty() ? Rgb{235, 232, 225} : medianRgb(paper);
            Image out(w, h);
            for (size_t i = 0; i < out.px.size(); i += 4)
                std::copy_n(std::array{c[0], c[1], c[2], 255.0f}.data(), 4, out.px.data() + i);
            return out;
        }

        // Step 6: the window, to the poster's size, lettered (text), in its
        // paper edge (frame) and taped (tape); or, for "?", blank paper of
        // that size with a big ? on it.
        Image finish(Image p, const std::string &title, const std::string &game, const Edges &edges,
                     const std::pair<Image, Image> &strips)
        {
            if (title == "?")
            {
                p = blank(edges, p.w, p.h);
                const Colours col = coloursOf(game);
                const int pad = int(float(p.w) * 0.12f);
                // Rokk has no ?.
                const Font big = fit(helvetica(), p.h / 2, "?", p.w, pad);
                letter(p, p.w / 2, (p.h - big.caps()) / 2, "?", big, col.name, col.line, 6);
            }
            else
                text(p, title, game);
            frame(p, edges);
            if (strips.first.w > 0)
                tape(p, strips);
            return p;
        }

        // GH2's or the 80s': a straight crop of venue select's picture, the
        // drawing and ribbon where and as big as the scene has them, the
        // window sized round them and slid to stay on the map.
        std::map<std::string, Image> mapPosters(const std::string &game)
        {
            std::map<std::string, Image> out;
            const auto disc = games::disc(game), gh2 = games::disc("gh2");
            if (!disc || !gh2)
                return out;
            const auto sc = scene(*disc, "ui/gen/sel_venue.milo_ps2");
            const auto gh2Scene = game == "gh2" ? sc : scene(*gh2, "ui/gen/sel_venue.milo_ps2");
            if (!sc || !gh2Scene)
                return out;
            // 1. The map, one picture down all its pages.
            const auto field = fieldOf(*gh2Scene);
            const auto stripped = field ? stripOf(*sc, *field) : std::nullopt;
            if (!stripped)
                return out;
            const auto &[base, top] = *stripped;
            const Unpainted blank(base);
            // 2. The paper strips: GH2's from its top, bottom and a middle
            // page; the 80s' top page is short, so its bottom's, flipped.
            Edges edges;
            {
                const auto p1 = sc->tex("sv_map1.tex"), p3 = sc->tex("sv_map3.tex");
                const auto p6 = game == "gh2" ? sc->tex("sv_map6.tex") : std::nullopt;
                if (!p1 || !p3 || (game == "gh2" && !p6))
                    return out;
                const Image bottom = page512(*p1), mid = page512(*p3);
                const int mt = 46, mb = game == "gh2" ? 47 : 46, ml = game == "gh2" ? 41 : 43, mr = 41;
                edges.top = game == "gh2" ? picture::crop(page512(*p6), 0, 0, 512, mt)
                                          : picture::flipTopBottom(picture::crop(bottom, 0, 512 - mt, 512, 512));
                edges.bottom = picture::crop(bottom, 0, 512 - mb, 512, 512);
                edges.left = picture::crop(mid, 0, 0, ml, 512);
                edges.right = picture::crop(mid, 512 - mr, 0, 512, 512);
            }
            const auto strips = tapes();
            out[kUnknown] = finish(Image(kPaperW, kPaperH), "?", game, edges, strips);
            constexpr Rgb kInk80s = {0.86f, 0.95f, 0.12f};
            const float w = kPaperW, h = kPaperH;
            const float loX = 46, hiX = 466;
            for (const Venue &v : game == "gh2" ? kGh2 : kGh80s)
            {
                const std::string art = v.art;
                const auto a = sc->xf("sv_venue_" + art + ".mesh");
                const auto drawing = sc->tex("sv_venue_" + art + ".tex");
                if (!a || !drawing)
                    continue;
                // 3. The drawing and its ribbon laid where the map has them,
                // on one layer over the stretch of map they span.
                struct Piece
                {
                    Image im;
                    int x, y;
                };
                std::vector<Piece> pieces;
                const auto put = [&](const Image &img, const std::array<float, 12> &m, float sx, float sy, bool ink)
                {
                    Image im = picture::resize(img, std::max(1, int(sx * kPx)), std::max(1, int(sy * kPx)));
                    if (ink)
                        inked(im, kInk80s);
                    pieces.push_back({std::move(im), 0, 0});
                    Piece &p = pieces.back();
                    p.x = int((m[9] + 100.0f) * kPx - float(p.im.w) / 2.0f);
                    p.y = int((top - m[11]) * kPx - float(p.im.h) / 2.0f);
                };
                put(*drawing, *a, 128 * (*a)[0], 128 * (*a)[5], game == "gh80s");
                const auto b = sc->xf("banner_" + art + ".mesh");
                const auto ribbon = sc->tex("banner_" + art + ".tex");
                if (b && ribbon)
                    put(*ribbon, *b, 256 * (*b)[0], 128 * (*b)[8], false);
                int lx = 1 << 30, ly = 1 << 30, hx = -(1 << 30), hy = -(1 << 30);
                for (const Piece &p : pieces)
                    lx = std::min(lx, p.x), ly = std::min(ly, p.y), hx = std::max(hx, p.x + p.im.w),
                    hy = std::max(hy, p.y + p.im.h);
                Image layer(hx - lx, hy - ly);
                for (const Piece &p : pieces)
                    picture::over(layer, p.im, p.x - lx, p.y - ly);
                const Box bb = picture::bbox(layer);
                const Image own = picture::crop(layer, bb.x0, bb.y0, bb.x1, bb.y1);
                const float x0 = float(lx + bb.x0), y0 = float(ly + bb.y0), x1 = float(lx + bb.x1), y1 = float(ly + bb.y1);
                // 4. The window: the poster's shape, as wide as makes the
                // drawing fill the band, cut from the map round it.
                const float band = (kBand1 - kBand0) * 0.86f;
                const float cw = std::min({(x1 - x0) / kRoom, (y1 - y0) / band * w / h, hiX - loX});
                const float ch = cw * h / w;
                const float idealX = (x0 + x1) / 2 - cw / 2, idealY = (y0 + y1) / 2 - ch * (kBand0 + kBand1) / 2;
                const float wx = std::max(loX, std::min(hiX - cw, idealX));
                // The map behind, slid up or down to stay on its paint.
                float wy = std::max(0.0f, std::min(float(base.h) - ch, idealY));
                float bestGap = 1e18f;
                for (int c = 0; c <= int(float(base.h) - ch); c += 4)
                    if (blank.painted(c, int(float(c) + ch), int(wx), int(wx + cw)) && std::fabs(float(c) - idealY) < bestGap)
                        bestGap = std::fabs(float(c) - idealY), wy = float(c);
                Image pic = picture::crop(base, int(wx), int(wy), int(wx + cw), int(wy + ch));
                // 5. The drawing over it, as venue select has it: not resized, not moved across; up
                // or down only as far as keeps it in the band.
                const float trueX = x0 - wx, trueY = y0 - wy;
                const float topY = ch * kBand0, bottomY = ch * kBand1;
                const float y = float(own.h) < bottomY - topY ? std::max(topY, std::min(bottomY - float(own.h), trueY))
                                                               : (topY + bottomY - float(own.h)) / 2;
                picture::over(pic, own, int(trueX), int(y));
                // 6. To the poster's size, lettered, framed and taped.
                out[v.venue] = finish(picture::resize(pic, kPaperW, kPaperH, picture::Filter::Lanczos), v.title, game,
                                      edges, strips);
            }
            return out;
        }

        // GH1's: its map's three pages, 650 units square at x 0, z 125,
        // 775 and 1425 (sel_venue.gh, local xfm at 8 in each Mesh), one
        // picture across them, each venue's drawing at its own place and
        // stretch; each poster cut from it round its venue, kept inside the
        // paint, its drawing at its own size.
        std::map<std::string, Image> gh1Posters()
        {
            std::map<std::string, Image> out;
            const auto disc = games::disc("gh1");
            if (!disc)
                return out;
            const auto img = [&](const std::string &n) { return bitmapFile(*disc, "ghui/image/gen/" + n); };
            const float U = 512.0f / 650.0f, top = 1425 + 325, left = -325;
            // 1. The map, its three pages one picture; 2. the paper strips
            // from their edges.
            Image full(int(650 * U), int(650 * 3 * U));
            Edges edges;
            {
                std::array<Image, 3> pages;
                for (int i = 0; i < 3; ++i)
                {
                    const auto m = img("map" + std::to_string(i + 1) + ".bmp_ps2");
                    if (!m)
                        return out;
                    pages[size_t(i)] = page512(*m);
                }
                for (const auto &[i, z] : {std::pair{0, 125.0f}, std::pair{1, 775.0f}, std::pair{2, 1425.0f}})
                    picture::over(full, pages[size_t(i)], 0, int((top - z - 325) * U));
                edges.top = picture::crop(pages[2], 0, 0, 512, 16);
                edges.bottom = picture::crop(pages[0], 0, 512 - 26, 512, 512);
                edges.left = picture::crop(pages[1], 0, 0, 15, 512);
                edges.right = picture::crop(pages[1], 512 - 22, 0, 512, 512);
            }
            struct Place
            {
                float x, z, sx, sz;
            };
            const std::map<std::string, Place> places = {
                {"basement", {-148, -46, 1.0f, 0.9f}}, {"smallclub", {116, 255, 1.2f, 1.05f}},
                {"bigclub", {-190, 636, 1.5f, 1.5f}},  {"theater", {81.7f, 826, 2.05f, 1.6f}},
                {"fest", {-113, 1196, 2.0f, 2.0f}},    {"arena", {0, 1555, 3.6f, 2.5f}},
            };
            const auto strips = tapes();
            out[kUnknown] = finish(Image(kPaperW, kPaperH), "?", "gh1", edges, strips);
            // Laid out at 4 times the map's own pixels, as the drawings are.
            constexpr float Z = 4;
            const float w = kPaperW, h = kPaperH;
            const float bigW = float(full.w) * Z, bigH = float(full.h) * Z;
            const float edge = 26 * Z, loX = edge, hiX = bigW - edge, loY = 48 * Z, hiY = bigH - 48 * Z;
            for (const Venue &v : kGh1)
            {
                // 3. The drawing, at its place and stretch on the map.
                const Place pl = places.at(v.art);
                const auto dr0 = img(std::string("venue_") + v.art + ".png_ps2");
                if (!dr0)
                    continue;
                const int dw = int(128 * pl.sx * U * Z), dh = int(128 * pl.sz * U * Z);
                const Image dr = picture::resize(*dr0, dw, dh);
                const float cx = (pl.x - left) * U * Z, cy = (top - pl.z) * U * Z;
                const Box bb = picture::bbox(dr);
                const Image drawn = picture::crop(dr, bb.x0, bb.y0, bb.x1, bb.y1);
                const float x0 = cx - float(dw) / 2 + float(bb.x0), y0 = cy - float(dh) / 2 + float(bb.y0);
                const float x1 = cx - float(dw) / 2 + float(bb.x1), y1 = cy - float(dh) / 2 + float(bb.y1);
                // 4. The window: as wide as makes the drawing fill the band,
                // the drawing in the band's middle; where that runs off the
                // map, the window slides, and the drawing with it.
                const float band = (kBand1 - kBand0) * 0.7f;
                float cw = std::min(std::min((x1 - x0) / (kRoom * 0.9f), (y1 - y0) / band * w / h), hiX - loX);
                float ch = cw * h / w;
                if (ch > hiY - loY)
                    ch = hiY - loY, cw = ch * w / h;
                const float idealX = (x0 + x1) / 2 - cw / 2, idealY = (y0 + y1) / 2 - ch * (kBand0 + kBand1) / 2;
                const float wx = std::max(loX, std::min(hiX - cw, idealX));
                const float wy = std::max(loY, std::min(hiY - ch, idealY));
                // The map's own pixels round the window, scaled up: as the
                // whole map scaled up and cut.
                const int sx0 = std::max(0, int(std::floor(wx / Z)) - 3), sy0 = std::max(0, int(std::floor(wy / Z)) - 3);
                const int sx1 = std::min(full.w, int(std::ceil((wx + cw) / Z)) + 3);
                const int sy1 = std::min(full.h, int(std::ceil((wy + ch) / Z)) + 3);
                const Image part = picture::crop(full, sx0, sy0, sx1, sy1);
                const Image up = picture::resize(part, part.w * int(Z), part.h * int(Z));
                Image pic = picture::crop(up, int(wx) - sx0 * int(Z), int(wy) - sy0 * int(Z),
                                          int(wx + cw) - sx0 * int(Z), int(wy + ch) - sy0 * int(Z));
                // 5. Its drawing over it, nudged only as far as keeps it in the band and
                // off the sides.
                const float pad = cw * (1 - kRoom) / 2 * 0.6f;
                const float dx = std::max(pad, std::min(cw - pad - float(drawn.w), x0 - wx));
                const float dy = std::max(ch * kBand0, std::min(ch * kBand1 - float(drawn.h), y0 - wy));
                picture::over(pic, drawn, int(dx), int(dy));
                // 6. To the poster's size, lettered, framed and taped.
                out[v.venue] = finish(picture::resize(pic, kPaperW, kPaperH, picture::Filter::Lanczos), v.title, "gh1",
                                      edges, strips);
            }
            return out;
        }

        // Never destroyed: prepare's thread may outlive main's statics.
        struct Made
        {
            std::mutex mutex;
            std::map<std::string, std::map<std::string, std::shared_ptr<const TextureData>>> games; // under mutex
        };
        Made &s_made = *new Made;

        const std::map<std::string, std::shared_ptr<const TextureData>> &made(const std::string &game)
        {
            const std::lock_guard lock(s_made.mutex);
            if (const auto found = s_made.games.find(game); found != s_made.games.end())
                return found->second;
            const auto posters = game == "gh1" ? gh1Posters() : mapPosters(game);
            auto &out = s_made.games[game];
            for (const auto &[venue, image] : posters)
            {
                auto data = std::make_shared<TextureData>();
                data->width = uint32_t(image.w);
                data->height = uint32_t(image.h);
                data->rgba = picture::toRgba(image);
                out[venue] = std::move(data);
            }
            return out;
        }
    }

    const std::vector<Venue> &venues(const std::string &game)
    {
        static const std::vector<Venue> none;
        return game == "gh2" ? kGh2 : game == "gh80s" ? kGh80s : game == "gh1" ? kGh1 : none;
    }

    std::shared_ptr<const TextureData> poster(const std::string &game, const std::string &venue)
    {
        const auto &all = made(game);
        const auto found = all.find(venue);
        return found != all.end() ? found->second : nullptr;
    }

    void prepare()
    {
        std::thread([]
                    {
                        for (const char *game : {"gh2", "gh80s", "gh1"})
                            made(game);
                    })
            .detach();
    }
}
