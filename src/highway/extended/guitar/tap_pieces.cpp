#include "highway/extended/guitar/tap_pieces.h"

#include "highway/gem_looks.h"
#include "milo/milo.h"
#include "render/hsv.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iostream>
#include <tuple>
#include <vector>

namespace gh2::guitar
{
    namespace
    {
        const Addresses *s_addresses = nullptr;

        constexpr float kTapAlpha = 0.5f, kGlassValue = 0.3f, kCyanStarValue = 0.75f;
        // Star power's cyan.
        constexpr float kCyan[3] = {0.3f, 1.0f, 1.0f};
        // A colour this pale (saturation) and light (value) is the lip's.
        constexpr float kLipSaturation = 0.25f, kLipValue = 0.5f;
        // A body colour's value this low stays glass, this high is the
        // lane's colour, eased between.
        constexpr float kRimFrom = 0.5f, kRimTo = 0.85f;
        struct Lane
        {
            const char *name;
            float hue; // degrees
        };
        constexpr Lane kLanes[] = {{"green", 120.0f}, {"red", 0.0f}, {"yellow", 55.0f}, {"blue", 215.0f}, {"orange", 30.0f}};

        // The materials whose colour is darkened at run time (every tap_
        // one but the lane gems', whose texture is), and each tap_ mesh's
        // material.
        // ...and the banded caps', see-through by their texture alone, and
        // those kept their colour, only see-through.
        std::set<std::string> s_darkened, s_caps;
        // An open's gem materials, glass.
        std::set<std::string> s_openGlass;
        std::map<std::string, std::string> s_meshMat;

        // A mesh copied as tap_<mesh>, on tap_<mat>.
        bool copyMesh(milo::Dir &scene, const std::string &mesh, const std::string &mat)
        {
            s_meshMat[tapped(mesh)] = tapped(mat);
            return addCopy(scene, mesh, tapped(mesh),
                           [&](milo::Bytes &body) { return milo::replaceStr(body, mesh, tapped(mesh)) && milo::replaceStr(body, mat, tapped(mat)); });
        }
        bool copyMat(milo::Dir &scene, const std::string &mat)
        {
            return addCopy(scene, mat, tapped(mat), [](milo::Bytes &) { return true; });
        }

        // A HOPO cap's top and the white base under it sample the same texels
        // of gem.tex, so a tap's cap is banded instead (as an open's,
        // highway/extended/guitar/open_gems.cpp): a texture of three bands down
        // it, the top its colour lit, the ring round it, and the base, white;
        // each vertex of its mesh's copy sampling its band, the shadow under it
        // gone. Colours as the PS2 draws a texture, at twice.
        constexpr float kCapTopSaturation = 0.6f;
        // Under star power, cyan through a tap's whole dome: lit, as star
        // power's are, where a lane's is dark.
        constexpr float kBonusGlow = 0.45f;
        constexpr float kCapTopV = 0.25f, kCapRingV = 0.625f, kCapBaseV = 0.875f;
        // Above the shadow, the base up to this height; the top above it,
        // inside the ring's corner of gem.tex (u past this).
        constexpr float kShadowUpTo = 0.02f, kBaseUpTo = 0.5f, kRingFrom = 0.45f;
        struct Cap
        {
            float top[3], ring[3], base[3];
        };

        bool band(milo::Bytes &body, uint32_t version, const Cap &cap)
        {
            const float *colours[3] = {cap.top, cap.ring, cap.base};
            return repaint(
                body, version,
                [&](size_t k, size_t, uint8_t *rgba)
                {
                    if (k >= 3u)
                        return;
                    for (int c = 0; c < 3; ++c)
                        rgba[c] = static_cast<uint8_t>(std::lround(std::clamp(colours[k][c] * 0.5f, 0.0f, 1.0f) * 255.0f));
                    rgba[3] = 0x80u; // whole, the PS2's alpha
                },
                [](uint32_t, uint32_t y, uint32_t, uint32_t h)
                { return static_cast<uint8_t>(y < h / 2u ? 0u : y < h * 3u / 4u ? 1u : 2u); });
        }

        // A cap mesh's vertices each on its band.
        bool banded(milo::Bytes &body)
        {
            const auto verts = vertices(body);
            if (!verts)
                return false;
            for (uint32_t i = 0; i < verts->count; ++i)
            {
                uint8_t *v = (*verts)[i];
                float z, u;
                std::memcpy(&z, v + 8u, 4u);
                std::memcpy(&u, v + kMeshVertexUv, 4u);
                const float h = z - verts->bottom;
                if (h < kShadowUpTo)
                {
                    std::memset(v, 0, 12u);
                    continue;
                }
                const float uv[2] = {0.5f, h < kBaseUpTo ? kCapBaseV : u > kRingFrom ? kCapRingV : kCapTopV};
                std::memcpy(v + kMeshVertexUv, uv, 8u);
            }
            return true;
        }

        // A banded cap: tap_<name>.mesh on its own Mat and Tex.
        bool copyCap(milo::Dir &scene, const std::string &mesh, const std::string &name, const Cap &cap)
        {
            const std::string tex = kTapPrefix + name + "_cap.tex", mat = kTapPrefix + name + "_cap.mat",
                              as = kTapPrefix + name + "_cap.mesh";
            bool ok = addCopy(scene, "gem.tex", tex, [&](milo::Bytes &body) { return band(body, scene.version, cap); });
            ok = addCopy(scene, "top.mat", mat, [&](milo::Bytes &body) { return milo::replaceStr(body, "gem.tex", tex); }) && ok;
            ok = addCopy(scene, mesh, as,
                         [&](milo::Bytes &body) { return milo::replaceStr(body, mesh, as) && milo::replaceStr(body, "top.mat", mat) && banded(body); }) &&
                 ok;
            s_meshMat[as] = mat;
            s_caps.insert(mat);
            return ok;
        }

        // gem.tex copied as `tex`, glass of a hue: its coloured body darkened
        // (kGlassValue) and eased into the hue at its lit sides, its pale lip
        // the hue; `glow`, the hue mixed through even its darkest glass.
        bool glassTex(milo::Dir &scene, const std::string &tex, float hue, float glow)
        {
            float colour[3];
            fromHsv(hue / 60.0f, 1.0f, 1.0f, colour);
            return addCopy(scene, "gem.tex", tex, [&](milo::Bytes &body)
                           {
                               return recolor(body, scene.version, [&](float *rgb)
                                              {
                                                  const float v = std::max({rgb[0], rgb[1], rgb[2]});
                                                  const float s = v > 0.0f ? (v - std::min({rgb[0], rgb[1], rgb[2]})) / v : 0.0f;
                                                  if (s < kLipSaturation && v > kLipValue)
                                                      return std::copy(colour, colour + 3, rgb), void();
                                                  const float t = std::max(glow, std::clamp((v - kRimFrom) / (kRimTo - kRimFrom), 0.0f, 1.0f));
                                                  for (int c = 0; c < 3; ++c)
                                                      rgb[c] = rgb[c] * kGlassValue * (1.0f - t) + colour[c] * t;
                                              });
                           });
        }

        // A gem as glass of a hue: `mesh` copied as `as` (tap_<mesh> unless
        // given), on a glassTex of its own.
        bool glassGem(milo::Dir &scene, const std::string &mesh, const std::string &name, float hue, const std::string &as = {},
                      float glow = 0.0f)
        {
            const std::string tex = kTapPrefix + name + "_gem.tex", mat = kTapPrefix + name + "_gem.mat";
            const std::string to = as.empty() ? tapped(mesh) : as;
            bool ok = glassTex(scene, tex, hue, glow);
            ok = addCopy(scene, "gem.mat", mat, [&](milo::Bytes &body) { return milo::replaceStr(body, "gem.tex", tex); }) && ok;
            ok = addCopy(scene, mesh, to, [&](milo::Bytes &body) { return milo::replaceStr(body, mesh, to) && milo::replaceStr(body, "gem.mat", mat); }) &&
                 ok;
            s_pieces.meshMat[to] = mat;
            return ok;
        }

        void addPieces(milo::Dir &scene)
        {
            int made = 0, wanted = 0;
            const auto count = [&](bool ok) { made += ok, ++wanted; };
            for (const Lane &lane : kLanes)
            {
                // The lane's gem: a texture of its own.
                count(glassGem(scene, lane.name + std::string("_gem.mesh"), lane.name, lane.hue));
                // Its star, as a phrase's gem: cyan glass, on star power's cyan
                // star (gameplay/engine/star_power_overlap) where that is made,
                // and that one's under star power.
                const std::string star = lane.name + std::string("_star.mesh");
                if (milo::find(scene, "cyan_star.mat"))
                {
                    count(addCopy(scene, star, tapped(star), [&](milo::Bytes &body)
                                  { return milo::replaceStr(body, star, tapped(star)) && milo::replaceStr(body, "star.mat", tapped("cyan_star.mat")); }));
                    s_meshMat[tapped(star)] = tapped("cyan_star.mat");
                }
                else
                    count(copyMesh(scene, star, "star.mat"));
                const std::string cyan = "cyan_" + std::string(lane.name) + "_star.mesh";
                if (milo::find(scene, cyan))
                    count(copyMesh(scene, cyan, "cyan_star.mat"));
                // Its HOPO cap: its top glass of its colour, lit, its ring
                // its colour, its base white.
                Cap cap{};
                fromHsv(lane.hue / 60.0f, kCapTopSaturation, 1.0f, cap.top);
                fromHsv(lane.hue / 60.0f, 1.0f, 1.0f, cap.ring);
                std::fill(std::begin(cap.base), std::end(cap.base), 1.0f);
                count(copyCap(scene, lane.name + std::string("_hopo.mesh"), lane.name, cap));
                // A star's HOPO top, its colour kept, see-through.
                count(copyMesh(scene, lane.name + std::string("_top_star.mesh"), "top.mat"));
                const std::string cyanTop = "cyan_" + std::string(lane.name) + "_top_star.mesh";
                if (milo::find(scene, cyanTop))
                    count(copyMesh(scene, cyanTop, "top.mat"));
            }
            // Under star power every lane's cap is the blue lane's: a tap's
            // star power's cyan.
            Cap bonus{};
            fromHsv(kStarPowerHue / 60.0f, kCapTopSaturation, 1.0f, bonus.top);
            std::copy(std::begin(kCyan), std::end(kCyan), bonus.ring);
            std::fill(std::begin(bonus.base), std::end(bonus.base), 1.0f);
            count(copyCap(scene, "blue_hopo.mesh", "bonus", bonus));
            // A star's base and shine, their colour kept, see-through.
            for (const char *mat : {"top.mat", "star_base.mat", "star2.mat"})
                count(copyMat(scene, mat));
            count(copyMesh(scene, "star_base.mesh", "star_base.mat"));
            count(copyMesh(scene, "star2.mesh", "star2.mat"));
            // Star power's gem, the same for every lane (gem_bonus.mesh, on
            // a flat blue of its own): a tap's the lanes' gem as cyan glass,
            // as a lane's is its colour's. And the stars' materials.
            count(glassGem(scene, "blue_gem.mesh", "bonus", kStarPowerHue, tapped("gem_bonus.mesh"), kBonusGlow));
            count(copyMat(scene, "star.mat"));
            for (const char *mat : {"star.mat"})
                s_darkened.insert(tapped(mat));
            if (milo::find(scene, "cyan_star.mat"))
            {
                count(copyMat(scene, "cyan_star.mat"));
                s_darkened.insert(tapped("cyan_star.mat"));
            }
            // An open tap's gem (highway/extended/guitar/open_gems: its body,
            // and its star's, on open_gem.mat, under star power
            // open_bonus_gem.mat) glass too, under its cap: purple, and star
            // power's cyan lit through, as tap_ copies open_gems shows for a
            // tap.
            for (const auto &[mat, from, hue, glow, meshes] :
                 {std::tuple{"open_gem.mat", "open.tex", kOpenHue, 0.0f, std::vector<std::string>{"open_gem.mesh", "open_star.mesh"}},
                  std::tuple{"open_bonus_gem.mat", "open_bonus.tex", kStarPowerHue, kBonusGlow, std::vector<std::string>{"open_gem_bonus.mesh"}}})
            {
                const std::string tex = tapped(std::string(mat).replace(std::strlen(mat) - 4u, 4u, ".tex"));
                count(glassTex(scene, tex, hue, glow));
                count(addCopy(scene, mat, tapped(mat), [&](milo::Bytes &body) { return milo::replaceStr(body, from, tex); }));
                for (const std::string &mesh : meshes)
                    count(copyMesh(scene, mesh, mat));
                s_pieces.openGlass.insert(tapped(mat));
            }
            if (made != wanted)
                std::cerr << "[custom] tap gems: " << made << " of " << wanted << " pieces made" << std::endl;
        }
    }

    std::string tapped(const std::string &name)
    {
        return kTapPrefix + name;
    }

    const TapPieces &tapPieces()
    {
        return s_pieces;
    }

    void addTapPieces()
    {
        addToTrackScene(addPieces);
    }
}
