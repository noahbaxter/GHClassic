#include "content/setlist_paper.h"

#include "guest.h"
#include "milo/layout.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <iostream>
#include <map>

namespace gh2::setlist_paper
{
    namespace
    {
        // GH2's paper as its scene lays it out: the songs' view slides 40
        // a frame (ss_songlist.tnm, 2800 by frame 70), the screen showing
        // 250 either side of -40 * frame + 300 in the paper's own; five
        // sheets 600 tall, centred 600 apart down from 0, of two textures
        // turn about; a leather wall behind whose texture repeats every
        // 425; twelve doodles down the margin. In a scene laid out so, the
        // slide goes on (kEndlessScale), and as the view goes past the
        // sheets they are put on again ahead of it, a sheet of each texture
        // in turn, the wall a repeat further on, and the doodles laid out
        // again down the margin (doodleAt), all while off screen: paper as
        // long as the list.
        constexpr float kRowLength = 40.0f, kSeenAt = 300.0f, kSeen = 550.0f;
        constexpr float kSheet = 600.0f, kWallRepeat = 425.0f;
        constexpr float kEndlessScale = 10000.0f;
        constexpr float kAway = 100000.0f; // where a sheet or doodle not needed goes
        constexpr const char *kSheets[] = {"setlist1.mesh", "setlist2.mesh", "setlist3.mesh", "setlist4.mesh",
                                           "setlist5.mesh"};
        constexpr int kDoodles = 12;
        // Each doodle's half width and half height on the paper as the
        // scene sizes it: a quad 200 across in its own units (as every
        // doodle's mesh is) through its transform, centred on its place.
        constexpr std::array<std::pair<float, float>, kDoodles> kDoodleSize = {{
            {70.0f, 140.0f}, {97.7f, 97.7f}, {207.9f, 117.0f}, {140.0f, 140.0f}, {80.0f, 80.0f}, {60.0f, 120.0f},
            {106.0f, 63.4f}, {80.0f, 80.0f}, {115.6f, 115.6f}, {80.0f, 80.0f}, {90.0f, 90.0f}, {50.0f, 50.0f},
        }};
        // The scene's doodles sit in the margin the printout (ghc_info_*,
        // ui/dta/quickplay.dta) covers. Here they go in a column from the
        // paper's red line to the printout's edge, each its right edge
        // against the printout, one too wide made smaller to fit, one
        // under another kDoodleGap apart from kDoodleTop down: a round of
        // all twelve, then again, each round in another order and every
        // other one mirrored.
        constexpr float kDoodleLeft = -150.0f, kDoodleRight = 115.0f;
        constexpr float kDoodleTop = 120.0f, kDoodleGap = 110.0f;

        float doodleScale(int i) { return std::min(1.0f, (kDoodleRight - kDoodleLeft) / (2.0f * kDoodleSize[i].first)); }
        float doodleHeight(int i) { return 2.0f * kDoodleSize[i].second * doodleScale(i) + kDoodleGap; }

        struct DoodlePlace
        {
            float x = 0.0f, z = 0.0f, scale = 1.0f;
            bool mirrored = false;
        };

        // Doodle i's place in the round nearest what is seen with the view
        // at `frame`.
        DoodlePlace doodleAt(int i, float frame)
        {
            float round = 0.0f;
            for (int k = 0; k < kDoodles; ++k)
                round += doodleHeight(k);
            const float seen = -kRowLength * frame + kSeenAt;
            const auto centre = [&](int r)
            {
                float z = kDoodleTop - round * static_cast<float>(r);
                for (int k = 0; k < kDoodles; ++k)
                {
                    const int d = (k + 5 * r) % kDoodles;
                    if (d == i)
                        return z - doodleHeight(d) / 2.0f;
                    z -= doodleHeight(d);
                }
                return z;
            };
            int best = std::max(0, static_cast<int>(std::floor((kDoodleTop - seen) / round)));
            for (int r = std::max(0, best - 1); r <= best + 1; ++r)
                if (std::fabs(centre(r) - seen) < std::fabs(centre(best) - seen))
                    best = r;
            const float scale = doodleScale(i);
            return {kDoodleRight - kDoodleSize[i].first * scale, centre(best), scale, best % 2 != 0};
        }

        // Doodle n's index from its name, -1 for another piece.
        int doodleIndex(const std::string &name)
        {
            for (int i = 0; i < kDoodles; ++i)
                if (name == "doodle" + std::to_string(i + 1) + ".mesh")
                    return i;
            return -1;
        }

        // Where each scene put each piece of its paper, x and z, as it
        // loaded, by the scene's path.
        using Pieces = std::map<std::string, std::pair<float, float>>;
        std::map<std::string, Pieces> s_paperBases;
        // The paper's frame at the list's row last framed, and the frames
        // above the window's first row.
        float s_paperFrame = 0.0f, s_listFrames = 0.0f;

        // Where a piece of paper goes with the view at `frame`, x and z.
        std::pair<float, float> paperAt(const Pieces &bases,const std::string &name, float frame)
        {
            const auto base = bases.find(name);
            if (base == bases.end())
                return {0.0f, kAway};
            const float seen = -kRowLength * frame + kSeenAt;
            const auto [x, z] = base->second;
            // The rows, laid out from the window's first, as far down as
            // the paper is past it.
            if (name == "ss_songlist.view")
                return {x, z - kRowLength * s_listFrames};
            if (name == "ss_bigwall.mesh")
                return {x, z - kWallRepeat * std::max(0.0f, std::round((z - seen) / kWallRepeat))};
            // Sheet k centred at -600k, its texture the first's where k is
            // even: the sheet of each in turn that covers what is seen.
            for (int m = 0; m < 5; ++m)
                if (name == kSheets[m])
                {
                    const int lo = std::max(0, static_cast<int>(std::ceil((-(seen + kSeen) - kSheet / 2.0f) / kSheet)));
                    const int hi = static_cast<int>(std::floor((-(seen - kSeen) + kSheet / 2.0f) / kSheet));
                    for (int k = lo; k <= hi; ++k)
                        if (k % 2 == m % 2 && (k / 2) % (m % 2 == 0 ? 3 : 2) == m / 2)
                            return {x, -kSheet * static_cast<float>(k)};
                    return {x, kAway};
                }
            if (const int i = doodleIndex(name); i >= 0)
            {
                const DoodlePlace at = doodleAt(i, frame);
                return {at.x, at.z};
            }
            return {x, kAway};
        }
    }

    void endlessPaper(milo::Dir &scene)
    {
        const auto tnm = milo::find(scene, "ss_songlist.tnm");
        if (!tnm || !milo::find(scene, "setlist5.mesh") || !milo::find(scene, "doodle12.mesh"))
            return;
        milo::Bytes &body = scene.bodies[*tnm];
        // Its two keys, position then frame: the last's z and frame.
        float z = 0.0f, frame = 0.0f;
        if (body.size() < 84u)
            return;
        std::memcpy(&z, body.data() + 76u, 4u);
        std::memcpy(&frame, body.data() + 80u, 4u);
        if (z != 2800.0f || frame != 70.0f)
        {
            std::cerr << "[setlist] ss_songlist.tnm not as GH2 keys it, its paper left as it is" << std::endl;
            return;
        }
        z *= kEndlessScale;
        frame *= kEndlessScale;
        std::memcpy(body.data() + 76u, &z, 4u);
        std::memcpy(body.data() + 80u, &frame, 4u);
    }

    std::optional<script::Node> command(const script::Call &call, const std::string &look,
                                        const std::vector<size_t> &tiers, size_t first)
    {
        const std::string op = call.symbol(1);
        // The paper's frame for the list's row: the setlist's whole
        // way down to it, a frame a row and one a header.
        if (op == "frame")
        {
            const size_t at = std::min(first + call.index(2),
                                       tiers.empty() ? size_t{0} : tiers.size() - 1u);
            size_t frames = at + (tiers.empty() ? 0u : 1u), before = 0u;
            for (size_t r = 1; r <= at && r < tiers.size(); ++r)
            {
                const bool header = tiers[r] != tiers[r - 1u];
                frames += header ? 1u : 0u;
                before += header && r <= first ? 1u : 0u;
            }
            s_paperFrame = static_cast<float>(frames);
            // The list lays its rows out from the window's first, its
            // header the window's own: the frames before that.
            s_listFrames = static_cast<float>(first + before);
            return script::Node{static_cast<uint32_t>(frames), script::kInt};
        }
        if (op == "paper_base")
        {
            s_paperBases[look].emplace(call.symbol(2), std::pair{call.number(3), call.number(4)});
            return script::Node{};
        }
        // A doodle's size and facing for its place now (doodleAt): its
        // local transform's first two rows, its axes across the paper,
        // as the scene had them, scaled to fit, the first turned about
        // where it is mirrored. Set before its place, which brings it
        // into the world.
        if (op == "paper_pose")
        {
            const int i = doodleIndex(call.symbol(2));
            const uint32_t object = call.object(3);
            if (i < 0 || object == 0u)
                return script::Node{};
            constexpr uint32_t kRow = 16u;
            // By look and doodle, as the scene's file has them: a scene
            // loaded again has the same, wherever its meshes now are.
            static std::map<std::pair<std::string, int>, std::array<float, 6>> s_axes;
            const uint32_t axes = object - ::milo::mesh::kObjectBase + ::milo::mesh::kTransform + ::milo::transformable::kLocal;
            auto found = s_axes.find({look, i});
            if (found == s_axes.end())
            {
                std::array<float, 6> own{};
                for (uint32_t k = 0; k < 6u; ++k)
                    own[k] = load<float>(call.rdram, axes + (k / 3u) * kRow + 4u * (k % 3u));
                found = s_axes.emplace(std::pair{look, i}, own).first;
            }
            const DoodlePlace at = doodleAt(i, s_paperFrame);
            for (uint32_t k = 0; k < 6u; ++k)
                store<float>(call.rdram, axes + (k / 3u) * kRow + 4u * (k % 3u),
                             found->second[k] * at.scale * (k < 3u && at.mirrored ? -1.0f : 1.0f));
            return script::Node{};
        }
        if (op == "paper")
        {
            const auto [x, z] = paperAt(s_paperBases[look], call.symbol(2), s_paperFrame);
            return script::floatNode(call.symbol(3) == "x" ? x : z);
        }
        return std::nullopt;
    }
}
