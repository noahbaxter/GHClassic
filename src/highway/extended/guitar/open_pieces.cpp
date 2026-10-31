// The pieces an open is drawn with (highway/extended/guitar/open_gems.h), made
// in the track's scene as it is read: GH2's own gem pulled apart at its middle
// to span the lanes and flattened, each part a wide open_<part>.mesh. A lane's
// colour is where its meshes' uvs fall on gem.tex, so the wide ones are made of
// the blue lane's, on gem.tex turned purple (cyan under star power); and the
// caps' streaks.

#include "highway/extended/guitar/open_pieces.h"

#include "highway/gem_looks.h"
#include "milo/layout.h"
#include "milo/milo.h"
#include "render/hsv.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iostream>
#include <iterator>

namespace gh2::guitar
{
    namespace
    {
        // Lanes are 4 apart, a gem 3.3 wide (TrackConfig::GetRawSlotCenter,
        // 0x1524f8): drawn on the middle lane, each half moves out to an
        // outer one. Flattened as Deluxe's kick is, its top at two thirds
        // of its height.
        constexpr float kHalfSpan = 8.0f, kFlatten = 0.67f;
        constexpr const char *kLaneColour = "yellow_";

        // Each wide mesh: the scene's mesh it is made of, and the material
        // it takes in place of that one's, if any. The middle lane's own
        // part X is drawn as open_X, a shared one as open_ and its name.
        struct Wide
        {
            const char *from, *as;
            const char *mat = nullptr, *openMat = nullptr;
            bool hidden = false; // none of it drawn
            bool across = false; // a cap: its uvs across it, on its streak's bands
            bool raised = false; // a HOPO's cap: its top lifted (kRaise)
            bool body = false;   // a gem's body: its top off its block's edge (kBodyTopV)
        };
        constexpr Wide kWide[] = {
            {.from = "blue_gem.mesh", .as = "open_gem.mesh", .mat = "gem.mat", .openMat = "open_gem.mat", .body = true},
            {.from = "blue_hopo.mesh", .as = "open_hopo.mesh", .mat = "top.mat", .openMat = "open_top.mat", .raised = true},
            {.from = "top.mesh", .as = "open_top.mesh", .mat = "top.mat", .openMat = "open_cap.mat", .across = true},
            {.from = "glow.mesh", .as = "open_glow.mesh"},
            {.from = "miss.mesh", .as = "open_miss.mesh"},
            {.from = "top_miss.mesh", .as = "open_top_miss.mesh"},
            {.from = "star_miss.mesh", .as = "open_star_miss.mesh"},
            {.from = "top_star_miss.mesh", .as = "open_top_star_miss.mesh"},
            // A star power gem's spins, a star, and has a base under it and
            // a star on top (black, or its lane's for a HOPO) and its shine
            // (star2, hidden for an open's, widenParts): an open's is its own
            // gem under its cap with the streak running along it, and none
            // of those.
            {.from = "blue_gem.mesh", .as = "open_star.mesh", .mat = "gem.mat", .openMat = "open_gem.mat", .body = true},
            {.from = "top.mesh", .as = "open_top_star_black.mesh", .mat = "top.mat", .openMat = "open_streak.mat",
             .across = true},
            {.from = "top.mesh", .as = "open_top_star.mesh", .mat = "top.mat", .openMat = "open_streak_hopo.mat",
             .across = true, .raised = true},
            {.from = "star_base.mesh", .as = "open_star_base.mesh", .hidden = true},
            // Under star power every lane's gem is its own blue one, under
            // the lane's cap or the blue lane's HOPO, and its glow: the
            // open's gem and lip are cyan.
            {.from = "blue_gem.mesh", .as = "open_gem_bonus.mesh", .mat = "gem.mat", .openMat = "open_bonus_gem.mat",
             .body = true},
            {.from = "gem_bonus2.mesh", .as = "open_gem_bonus2.mesh"},
            {.from = "top.mesh", .as = "open_bonus_top.mesh", .mat = "top.mat", .openMat = "open_on_cap.mat", .across = true},
            {.from = "top.mesh", .as = "open_bonus_hopo.mesh", .mat = "top.mat", .openMat = "open_on_hopo.mat",
             .across = true, .raised = true},
        };


        // A cap (top.mesh) is bands: a shadow flat on the track, the lip
        // round the gem's foot (the white edge) and the cap over it, the
        // gem between, the cap its flat top inside a ring (its own corner
        // of gem.tex, u past kRingFrom). Across, each takes its band of the
        // streak's texture (streak, below), its top the streak, and the
        // shadow goes. Heights above the shadow, as they were before
        // flattening.
        constexpr float kShadowUpTo = 0.02f, kLipUpTo = 0.5f, kRingFrom = 0.6f;
        constexpr float kTopV = 0.25f, kRingV = 0.625f, kLipV = 0.875f;
        // A raised cap's ring and top, above kRaiseFrom, lifted kRaise more,
        // so a HOPO stands taller than a strum, its ring's wall the longer.
        constexpr float kRaiseFrom = 0.7f, kRaise = 0.15f;
        // A gem body's foot samples gem.tex's grey band (v 0.517) and its
        // top ring the edge under it (0.639), both pale; under a gem's lip
        // and cap they are hidden, past a flattened open's ends they show.
        // Each taken inside its lane's block.
        constexpr float kBodyTopV = 0.68f;

        // The mesh pulled apart at its middle, a half to each outer lane,
        // and flattened toward the track, as every part is, so they meet as
        // they did; or, hidden, every vertex at its middle.
        bool widen(milo::Bytes &body, const Wide &wide)
        {
            const auto verts = vertices(body);
            if (!verts)
                return false;
            const uint32_t n = verts->count;
            const auto vertex = [&](uint32_t i) { return (*verts)[i]; };
            if (wide.hidden)
            {
                for (uint32_t i = 0; i < n; ++i)
                    std::memset(vertex(i), 0, 12u);
                return true;
            }
            const float bottom = verts->bottom;
            for (uint32_t i = 0; i < n; ++i)
            {
                uint8_t *v = vertex(i);
                float x, z, uvV;
                std::memcpy(&x, v, 4u);
                std::memcpy(&z, v + 8u, 4u);
                std::memcpy(&uvV, v + kMeshVertexUv + 4u, 4u);
                x += x > 0.0f ? kHalfSpan : -kHalfSpan;
                const bool lifted = wide.raised && z - bottom > kRaiseFrom;
                z = z * kFlatten + (lifted ? kRaise : 0.0f);
                if (wide.body)
                    uvV = std::max(uvV, kBodyTopV);
                std::memcpy(v, &x, 4u);
                std::memcpy(v + 8u, &z, 4u);
                std::memcpy(v + kMeshVertexUv + 4u, &uvV, 4u);
            }
            if (!wide.across)
                return true;
            // Each vertex's height as it was, above the lowest.
            const auto height = [&](const uint8_t *v)
            {
                float z;
                std::memcpy(&z, v + 8u, 4u);
                return z / kFlatten - bottom;
            };
            float lo = 1e9f, hi = -1e9f;
            for (uint32_t i = 0; i < n; ++i)
            {
                if (height(vertex(i)) < kShadowUpTo)
                    continue;
                float x;
                std::memcpy(&x, vertex(i), 4u);
                lo = std::min(lo, x), hi = std::max(hi, x);
            }
            for (uint32_t i = 0; i < n && hi > lo; ++i)
            {
                uint8_t *v = vertex(i);
                const float h = height(v);
                if (h < kShadowUpTo)
                {
                    std::memset(v, 0, 12u);
                    continue;
                }
                float x, u;
                std::memcpy(&x, v, 4u);
                std::memcpy(&u, v + kMeshVertexUv, 4u);
                const float uv[2] = {(x - lo) / (hi - lo), h < kLipUpTo ? kLipV : u > kRingFrom ? kRingV : kTopV};
                std::memcpy(v + kMeshVertexUv, uv, 8u);
            }
            return true;
        }

        // An open's cap: its top purple on a strummed one, cyan under star
        // power, and white on a HOPO, on a star power open's with a pulse
        // across it (u, wrapping), brightest at its middle, the same down
        // it, scrolled; its ring black on a strummed one, as a strummed
        // gem's is, or its gem's colour on a HOPO, as a HOPO's is; its lip
        // white, or cyan on a star power open's and under star power. Drawn
        // at twice its colour, as a texture is. Star power's cyan, as a star
        // gem's base is drawn (110, 250, 248), and the open's purple, as its
        // gem is (205, 105, 250).
        constexpr float kStreakCyan[3] = {0.22f, 0.49f, 0.49f}, kStreakPurple[3] = {0.4f, 0.21f, 0.49f},
                        kStreakWhite[3] = {0.5f, 0.5f, 0.5f}, kStreakBlack[3] = {0.0f, 0.0f, 0.0f};
        constexpr float kPulseSharpness = 3.0f;
        // Each cap's texture and material, its top, its pulse's colour (none
        // for none), its ring and its lip. A HOPO open's is its gem's
        // (open_hopo).
        struct Streak
        {
            const char *tex, *mat;
            const float *top, *pulse, *ring, *lip;
        };
        constexpr Streak kStreaks[] = {
            {"open_cap.tex", "open_cap.mat", kStreakPurple, nullptr, kStreakBlack, kStreakWhite},
            {"open_streak.tex", "open_streak.mat", kStreakPurple, kStreakCyan, kStreakBlack, kStreakCyan},
            {"open_streak_hopo.tex", "open_streak_hopo.mat", kStreakWhite, kStreakCyan, kStreakPurple, kStreakCyan},
            // Under star power.
            {"open_on_cap.tex", "open_on_cap.mat", kStreakCyan, nullptr, kStreakBlack, kStreakCyan},
            {"open_on_hopo.tex", "open_on_hopo.mat", kStreakWhite, nullptr, kStreakCyan, kStreakCyan},
        };
        void streakColor(float u, const Streak &s, uint8_t *rgba)
        {
            const float pulse = s.pulse ? std::pow(0.5f + 0.5f * std::cos(u * 6.2831853f), kPulseSharpness) : 0.0f;
            const float shine = std::pow(pulse, 4.0f) * 0.12f;
            for (int c = 0; c < 3; ++c)
            {
                const float to = s.pulse ? s.pulse[c] : s.top[c];
                rgba[c] = static_cast<uint8_t>(
                    std::lround(std::min(1.0f, s.top[c] + (to - s.top[c]) * pulse + shine) * 255.0f));
            }
            rgba[3] = 0x80u; // whole, the PS2's alpha
        }

        // A Tex body repainted as a cap: the top in its top half, then its
        // ring's colour and its lip's, a quarter each: its palette a ramp
        // across the wheel of the top but for its last two entries, those,
        // and every pixel of the top half the entry of its column.
        bool streak(milo::Bytes &body, uint32_t version, const Streak &s)
        {
            size_t ramp = 0u;
            return repaint(
                body, version,
                [&](size_t k, size_t entries, uint8_t *rgba)
                {
                    ramp = entries - 2u;
                    if (k < ramp)
                        return streakColor(static_cast<float>(k) / static_cast<float>(ramp), s, rgba);
                    const float *colour = k == ramp ? s.ring : s.lip;
                    for (int c = 0; c < 3; ++c)
                        rgba[c] = static_cast<uint8_t>(std::lround(colour[c] * 255.0f));
                    rgba[3] = 0x80u;
                },
                [&](uint32_t x, uint32_t y, uint32_t w, uint32_t h)
                { return static_cast<uint8_t>(y < h / 2u ? x * ramp / w : y < h * 3u / 4u ? ramp : ramp + 1u); });
        }

        // gem.tex with every colour's hue turned to `degrees`, its
        // saturation and value kept, so greys stay grey.
        bool toHue(milo::Bytes &body, uint32_t version, float degrees)
        {
            return recolor(body, version, [degrees](float *rgb)
                           {
                               const Hsv hsv = toHsv(rgb);
                               fromHsv(degrees / 60.0f, hsv.s, hsv.v, rgb);
                           });
        }
    }

    // The wide mesh drawn for a part of the middle lane's, of a rep of
    // `type`.
    std::string openName(const std::string &mesh, uint32_t type)
    {
        if (mesh == "blue_hopo.mesh")
            return "open_bonus_hopo.mesh";
        if (mesh == "top.mesh" && type == gemRep::kTypeBonus)
            return "open_bonus_top.mesh";
        return "open_" + (mesh.rfind(kLaneColour, 0u) == 0u ? mesh.substr(std::strlen(kLaneColour)) : mesh);
    }

    std::vector<std::string> openStreakMats()
    {
        std::vector<std::string> mats;
        for (const Streak &s : kStreaks)
            mats.push_back(s.mat);
        return mats;
    }

    // The track's scene with the open gem's pieces: gem.tex in purple,
    // and in cyan for under star power, the caps, the materials on them,
    // and the wide meshes.
    void addOpenPieces(milo::Dir &scene)
    {
        const auto onTex = [](const char *tex) { return [tex](milo::Bytes &body) { return milo::replaceStr(body, "gem.tex", tex); }; };
        int made = addCopy(scene, "gem.tex", "open.tex", [&](milo::Bytes &body) { return toHue(body, scene.version, kOpenHue); });
        made += addCopy(scene, "gem.mat", "open_gem.mat", onTex("open.tex"));
        made += addCopy(scene, "top.mat", "open_top.mat", onTex("open.tex"));
        made += addCopy(scene, "gem.tex", "open_bonus.tex", [&](milo::Bytes &body) { return toHue(body, scene.version, kStarPowerHue); });
        made += addCopy(scene, "gem.mat", "open_bonus_gem.mat", onTex("open_bonus.tex"));
        for (const Streak &s : kStreaks)
        {
            made += addCopy(scene, "gem.tex", s.tex, [&](milo::Bytes &body) { return streak(body, scene.version, s); });
            made += addCopy(scene, "top.mat", s.mat, onTex(s.tex));
        }
        for (const Wide &wide : kWide)
            // Its geometry its own, not the one it is a copy of (a mesh
            // names its owner).
            made += addCopy(scene, wide.from, wide.as, [&](milo::Bytes &body)
                            { return widen(body, wide) && milo::replaceStr(body, wide.from, wide.as) &&
                                     (!wide.mat || milo::replaceStr(body, wide.mat, wide.openMat)); });
        const size_t pieces = 5u + 2u * std::size(kStreaks) + std::size(kWide);
        if (made != static_cast<int>(pieces))
            std::cerr << "[custom] open gems: " << made << " of " << pieces << " pieces made" << std::endl;
    }
}
