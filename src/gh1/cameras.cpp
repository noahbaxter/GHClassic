// GH1's camera shots as GH2's.
//
// GH1 scripts a shot: {arena switch_cam <path> <name> (start 0) ...} in
// venues/<v>/camera.dta, a list of them to a group ($camedit.flr_near_lft).
// The camera rides <path>.tnm of campaths.rnd from start to end percent over
// duration ms, offset from it, under the path's parent and looking at its
// target (arena/cam_paths.dta; with none named, each is the player's head,
// VenueCam::GetTargetHead, GH1 0x16f548), which it holds at a spot on screen
// (VenueCam::OnSwitchCam, GH1 0x170840, and Poll, 0x16f830).
//
// GH2's CamShot keeps keys: each a place under a parent, targets it looks at
// and that spot on screen, the camera moving straight from one to the next
// (CamShotFrame::BuildTransform, 0x267008, and Interp, 0x2665a0: Poll's
// code still). So a GH1 shot is a CamShot of its group's category whose keys
// are its path sampled, and a still one a single key.
//
// GH1's shake track (shaky_cam1.tnm) is GH2's noise here. Its real_time
// clock, eyes, guard band, force_cam_facing and crowd region have nothing of
// GH2's to be and are left out, as are the groups GH2 never asks for
// (TUTORIAL, SINGER, FINAL_WIN_GRIM).

#include "gh1/cameras.h"

#include "disc/ark.h"
#include "formats/dtb.h"
#include "gh1/rig.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iostream>
#include <map>
#include <optional>
#include <set>

namespace gh2::gh1
{
    namespace
    {
        using milo::putStr;
        using milo::putU32;
        using milo::str;
        using milo::u32;

        float f32(const Bytes &b, size_t o)
        {
            float v = 0.0f;
            if (o + 4u <= b.size())
                std::memcpy(&v, b.data() + o, 4u);
            return v;
        }

        void putF32(Bytes &out, float v)
        {
            uint32_t u = 0u;
            std::memcpy(&u, &v, 4u);
            putU32(out, u);
        }

        struct Vec
        {
            float x = 0.0f, y = 0.0f, z = 0.0f;
        };

        Vec operator+(const Vec &a, const Vec &b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
        Vec operator-(const Vec &a, const Vec &b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
        Vec operator*(const Vec &a, float s) { return {a.x * s, a.y * s, a.z * s}; }

        // A camera path: GH1's TransAnim 4 (RndTransAnim::Load, GH1
        // 0x1e9808), its position keys and rotation keys by frame.
        struct Path
        {
            struct Turn
            {
                float q[4];
                float frame;
            };
            std::vector<std::pair<Vec, float>> keys;
            std::vector<Turn> turns;
            bool spline = false;
        };

        // Past a count and that many names.
        void skipNames(const Bytes &b, size_t &o)
        {
            const uint32_t n = u32(b, o);
            o += 4u;
            for (uint32_t i = 0; i < n && o < b.size(); ++i)
                str(b, o);
        }

        std::optional<Path> path(const Bytes &b)
        {
            // Anim 0 with no filters and its children, Draw 1 (showing, its
            // children, a sphere), the object it moves, then the keys:
            // rotations (a quaternion, a frame), positions (a point, a
            // frame), their owner and whether positions are a spline.
            if (u32(b, 0u) != 4u || u32(b, 8u) != 0u)
                return std::nullopt;
            size_t o = 12u;
            skipNames(b, o);
            o += 5u;
            skipNames(b, o);
            o += 16u;
            str(b, o);
            Path out;
            const uint32_t turns = u32(b, o);
            o += 4u;
            if (o + static_cast<size_t>(turns) * 20u + 4u > b.size())
                return std::nullopt;
            for (uint32_t i = 0; i < turns; ++i, o += 20u)
                out.turns.push_back({{f32(b, o), f32(b, o + 4u), f32(b, o + 8u), f32(b, o + 12u)}, f32(b, o + 16u)});
            const uint32_t keys = u32(b, o);
            o += 4u;
            if (keys == 0u || o + static_cast<size_t>(keys) * 16u > b.size())
                return std::nullopt;
            for (uint32_t i = 0; i < keys; ++i, o += 16u)
                out.keys.push_back({{f32(b, o), f32(b, o + 4u), f32(b, o + 8u)}, f32(b, o + 12u)});
            str(b, o);
            out.spline = o < b.size() && b[o] != 0u;
            return out;
        }

        // A spline's tangent at key `i` of three or more (GH1 0x268670).
        Vec tangent(const Path &p, size_t i)
        {
            const auto &k = p.keys;
            const size_t n = k.size();
            if (i == 0u)
                return (k[1].first - k[0].first) * 1.5f - (k[2].first - k[0].first) * 0.25f;
            if (i == n - 1u)
                return (k[i].first - k[n - 2u].first) * 1.5f - (k[i].first - k[n - 3u].first) * 0.25f;
            return (k[i + 1u].first - k[i - 1u].first) * 0.5f;
        }

        // Where a path is at that frame (InterpVector, GH1 0x268a58): on a
        // key, or between two along a Hermite curve, else a straight line.
        Vec position(const Path &p, float frame)
        {
            const auto &k = p.keys;
            if (frame <= k.front().second)
                return k.front().first;
            if (frame >= k.back().second)
                return k.back().first;
            size_t hi = 1u;
            while (k[hi].second < frame)
                ++hi;
            const size_t lo = hi - 1u;
            const float t = (frame - k[lo].second) / (k[hi].second - k[lo].second);
            if (!p.spline)
                return k[lo].first + (k[hi].first - k[lo].first) * t;
            const float t2 = t * t, t3 = t2 * t;
            const Vec in = k.size() > 2u ? tangent(p, lo) : k[1].first - k[0].first;
            const Vec out = k.size() > 2u ? tangent(p, hi) : in;
            return k[lo].first * (2.0f * t3 - 3.0f * t2 + 1.0f) + in * (t3 - 2.0f * t2 + t) +
                   k[hi].first * (3.0f * t2 - 2.0f * t3) + out * (t3 - t2);
        }

        // How a path is turned at that frame, as a matrix's rows
        // (MakeRotMatrix, GH1 0x2667d8): the keys either side, blended and
        // made unit (FastInterp, GH1 0x266398).
        void rotation(const Path &p, float frame, float m[9])
        {
            float q[4] = {0.0f, 0.0f, 0.0f, 1.0f};
            const auto &k = p.turns;
            if (!k.empty())
            {
                size_t hi = 0u;
                while (hi + 1u < k.size() && k[hi].frame < frame)
                    ++hi;
                const size_t lo = hi > 0u && frame < k[hi].frame ? hi - 1u : hi;
                const float span = k[hi].frame - k[lo].frame;
                const float t = span > 0.0f ? (frame - k[lo].frame) / span : 0.0f;
                float dot = 0.0f;
                for (int i = 0; i < 4; ++i)
                    dot += k[lo].q[i] * k[hi].q[i];
                float length = 0.0f;
                for (int i = 0; i < 4; ++i)
                {
                    q[i] = k[lo].q[i] + ((dot < 0.0f ? -k[hi].q[i] : k[hi].q[i]) - k[lo].q[i]) * t;
                    length += q[i] * q[i];
                }
                length = std::sqrt(length);
                for (float &v : q)
                    v = length > 0.0f ? v / length : v;
            }
            const float x = q[0], y = q[1], z = q[2], w = q[3];
            const float rows[9] = {1.0f - 2.0f * (y * y + z * z), 2.0f * (x * y + z * w), 2.0f * (x * z - y * w),
                                   2.0f * (x * y - z * w), 1.0f - 2.0f * (x * x + z * z), 2.0f * (y * z + x * w),
                                   2.0f * (x * z + y * w), 2.0f * (y * z - x * w), 1.0f - 2.0f * (x * x + y * y)};
            std::copy(rows, rows + 9, m);
        }

        // One of GH1's shots: its group, path, name and parameters.
        struct Shot
        {
            std::string group, path, name;
            dtb::Node params;

            float number(const std::string &key, size_t i, float fallback) const
            {
                const dtb::Node *found = dtb::find(params, key);
                const auto v = found && found->nodes.size() > i + 1u ? dtb::number(found->nodes[i + 1u]) : std::nullopt;
                return v ? *v : fallback;
            }

            Vec vec(const std::string &key) const { return {number(key, 0u, 0.0f), number(key, 1u, 0.0f), number(key, 2u, 0.0f)}; }
        };

        // A venue's shots, in the order its script lists them.
        std::vector<Shot> shots(const dtb::Node &script)
        {
            std::vector<Shot> out;
            static const std::string kList = "camedit.";
            for (const dtb::Node &set : script.nodes)
            {
                if (set.type != dtb::kCommand || set.nodes.size() < 3u || set.nodes[1].type != dtb::kVar)
                    continue;
                const size_t at = set.nodes[1].text.find(kList);
                if (at == std::string::npos)
                    continue;
                for (const dtb::Node &shot : set.nodes[2].nodes)
                    if (shot.type == dtb::kCommand && shot.nodes.size() > 3u && shot.nodes[1].text == "switch_cam")
                        out.push_back({set.nodes[1].text.substr(at + kList.size()), shot.nodes[2].text, shot.nodes[3].text, shot});
            }
            return out;
        }

        // What a path looks at or hangs under ("arena::venue.view"): as it
        // is written, or the first choice of an {if_else {exists a} a b}.
        std::string named(const dtb::Node &paths, const std::string &path, const std::string &key)
        {
            const dtb::Node *entry = dtb::find(paths, path);
            const dtb::Node *found = entry ? dtb::find(*entry, key) : nullptr;
            if (!found || found->nodes.size() < 2u)
                return {};
            const dtb::Node &value = found->nodes[1];
            return value.type == dtb::kCommand && value.nodes.size() > 2u ? value.nodes[2].text : value.text;
        }

        // GH2's ShotTarget (ShotTarget::Load, 0x261d40): an object of the
        // shot's dirs, and one inside it if that is a dir itself.
        struct Target
        {
            std::string entity, part;
        };

        void put(Bytes &out, const Target &t)
        {
            putU32(out, 20u);
            putStr(out, t.entity);
            putStr(out, t.part);
        }

        // The objects the shots name. GH1's player's head is a point 24 over
        // its base (cam_singer_height, GH1 0x16f2f0), which its shots frame
        // as neck high: here the neck, as GH2's own shots follow it.
        const char *const kGuitarists[] = {"guitarist0", "guitarist1"};
        const char *const kHead = "bone_neck.mesh";
        const char *const kSpot = "stage_spot_01.mesh";

        // How long z is in the spot on GH1's guitar necks that a shot hangs
        // under (charsys/guitars/guitar_*.rnd, spot_neck_fret01.mesh's local),
        // which scales the shot's place with it. GH2's guitars' is 1.
        constexpr float kSpotDepth = 0.362f;

        // GH1's "namespace::object" as a ShotTarget. Its venue's own view
        // sits at the origin unturned in every venue, so is no parent.
        Target target(const std::string &gh1)
        {
            const size_t colons = gh1.find("::");
            if (colons == std::string::npos || gh1 == "arena::venue.view")
                return {};
            const std::string space = gh1.substr(0, colons), object = gh1.substr(colons + 2u);
            return space == "arena" ? Target{object, {}} : Target{space, object};
        }

        // GH2's Trans 9 at that world, under no parent: Hmx::Object's
        // header, its local and world, then no constraint, target or parent.
        Bytes trans(const Bytes &world)
        {
            Bytes out;
            putU32(out, 9u);
            putU32(out, 0u);
            putStr(out, {});
            out.push_back(0u);
            for (int i = 0; i < 2; ++i)
                out.insert(out.end(), world.begin(), world.end());
            putU32(out, 0u);
            putStr(out, {});
            out.push_back(0u);
            putStr(out, {});
            return out;
        }

        // A DataArray's nodes (DataNode::Load, 0x2b8778).
        void putSymbol(Bytes &out, const std::string &s)
        {
            putU32(out, 5u);
            putStr(out, s);
        }

        void putInt(Bytes &out, int32_t v)
        {
            putU32(out, 0u);
            putU32(out, static_cast<uint32_t>(v));
        }

        void putSize(Bytes &out, size_t nodes)
        {
            out.push_back(static_cast<uint8_t>(nodes));
            out.push_back(static_cast<uint8_t>(nodes >> 8));
            out.insert(out.end(), 4u, 0u);
        }

        // What GH2 picks a shot by (camshot.dta), from GH1's group flags
        // (arena/camera.dta's $camera.groups) and its pool's weight.
        struct Kind
        {
            const char *distance, *facing, *solo;
            bool special;
            float weight;
        };

        const Kind *kind(const std::string &category)
        {
            static const std::map<std::string, Kind> kKinds = {
                {"flr_near_lft", {"near", "left", "never", false, 0.5f}},
                {"flr_near_rt", {"near", "right", "never", false, 0.5f}},
                {"flr_far_lft", {"far", "left", "never", false, 0.5f}},
                {"flr_far_rt", {"far", "right", "never", false, 0.5f}},
                {"band_POV", {"behind", "null", "never", false, 0.2f}},
                {"balcony_lft", {"far", "left", "never", false, 0.3f}},
                {"balcony_rt", {"far", "right", "never", false, 0.3f}},
                {"SOLO_NEAR", {"closeup", "null", "only", false, 1.0f}},
                {"SOLO_FAR", {"near", "null", "only", false, 1.0f}},
                {"LOSE", {"null", "null", "ok", true, 1.0f}},
                {"WIN", {"null", "null", "ok", true, 1.0f}},
                {"INTRO", {"null", "null", "ok", true, 1.0f}},
                {"INTRO_FAST", {"null", "null", "ok", true, 1.0f}},
                {"INTRO_ENCORE", {"null", "null", "ok", true, 1.0f}},
                {"WIN_ENCORE", {"null", "null", "ok", true, 1.0f}},
                {"WIN_ENCORE_SONG", {"null", "null", "ok", true, 1.0f}},
                {"WIN_GAME", {"null", "null", "ok", true, 1.0f}},
                {"LIGHTER", {"far", "null", "never", true, 1.0f}},
            };
            const auto it = kKinds.find(category);
            return it != kKinds.end() ? &it->second : nullptr;
        }

        // GH1's field of view is across the screen and GH2's up it, both of
        // a 4:3 picture.
        float fov(float degrees)
        {
            return 2.0f * std::atan(0.75f * std::tan(degrees * 0.0174533f * 0.5f));
        }

        // How far along a move of that ease GH1 is at `t` of its time: even,
        // or by ATanInterpolator (GH1 0x264440 and 0x264640).
        float eased(float ease, float t)
        {
            return ease != 0.0f ? 0.5f + std::atan(ease * (2.0f * t - 1.0f)) / (2.0f * std::atan(ease)) : t;
        }

        // A GH1 shot as a CamShot 20 (CamShot::Load, 0x264528) of that
        // category, for one player or two: all of it, or only where it ends.
        std::optional<Bytes> camShot(const Shot &shot, const std::string &category, bool ended, bool two,
                                     const std::map<std::string, Path> &paths, const dtb::Node &camPaths,
                                     const Stage &stage)
        {
            const Kind *as = kind(category);
            const auto on = paths.find(shot.path);
            if (!as || on == paths.end())
                return std::nullopt;
            const Path &path = on->second;

            Bytes out;
            putU32(out, 20u);
            // Hmx::Object's header (0x2c2018): the type and its properties.
            putU32(out, 0u);
            putStr(out, "band");
            out.push_back(1u);
            // The walk spots GH1 keeps the shot from (bad_walk_spots, each
            // by its number less one) as the waypoints on them, which GH2
            // keeps it from by the one nearest the guitarist (0x11f628).
            std::vector<std::string> bad;
            if (const dtb::Node *spots = dtb::find(shot.params, "bad_walk_spots"); spots && spots->nodes.size() > 1u)
                for (const dtb::Node &spot : spots->nodes[1].nodes)
                    if (const auto walk = static_cast<size_t>(spot.integer); spot.type == dtb::kInt && walk < stage.walks.size())
                        bad.insert(bad.end(), stage.walks[walk].begin(), stage.walks[walk].end());
            const bool hideCrowd = shot.number("hide_crowd", 0u, 0.0f) != 0.0f;
            putSize(out, bad.empty() ? 18u : 20u);
            putSymbol(out, "distance");
            putSymbol(out, as->distance);
            putSymbol(out, "facing");
            putSymbol(out, as->facing);
            putSymbol(out, "special");
            putInt(out, as->special);
            putSymbol(out, "solo");
            putSymbol(out, as->solo);
            putSymbol(out, "hide_crowd");
            putInt(out, hideCrowd);
            putSymbol(out, "force_char_lod");
            putInt(out, static_cast<int32_t>(shot.number("force_char_lod", 0u, -1.0f)));
            putSymbol(out, "walk_ok");
            putInt(out, shot.number("walk_ok", 0u, 1.0f) != 0.0f);
            putSymbol(out, "low_excitement_ok");
            putInt(out, shot.number("low_excitement_ok", 0u, 1.0f) != 0.0f);
            // GH1 changes shot through star power as at any other time.
            putSymbol(out, "starpower_ok");
            putInt(out, 1);
            if (!bad.empty())
            {
                putSymbol(out, "bad_waypoints");
                putU32(out, 16u);
                putSize(out, bad.size());
                for (const std::string &waypoint : bad)
                {
                    putU32(out, 4u);
                    putStr(out, waypoint);
                }
            }
            // RndAnimatable 4 (0x1ab228): its frame, and 30 frames a second.
            putU32(out, 4u);
            putF32(out, 0.0f);
            putU32(out, 0u);

            // The path's frames this shot covers, start to end percent of
            // its length, and how long it takes in GH2's frames. The "in"
            // offset, screen spot and field of view are those at the lower
            // of the two frames, the "out" at the higher (GH1 0x16f9ec).
            const float length = path.keys.back().second;
            const float from = shot.number("start", 0u, 0.0f) * length / 100.0f;
            const float to = shot.number("end", 0u, 100.0f) * length / 100.0f;
            const float frames = shot.number("duration", 0u, 1920.0f) * 0.03f;
            const float ease = shot.number("ease", 0u, 0.0f);
            const bool still = ended || from == to || frames <= 0.0f;
            const int steps = still ? 0 : std::clamp(static_cast<int>(std::ceil(frames / 15.0f)), 4, 24);

            const std::string lookAt = named(camPaths, shot.path, "target"), under = named(camPaths, shot.path, "parent");
            std::vector<Target> targets;
            if (!lookAt.empty())
                targets.push_back(target(lookAt));
            else
                for (int i = 0; i < (two ? 2 : 1); ++i)
                    targets.push_back({kGuitarists[i], kHead});
            const Target parent = under.empty() ? Target{kGuitarists[0], kHead} : target(under);
            // A parent that is part of the guitar turns the shot with it;
            // the head only carries it.
            const bool turned = !under.empty() && !parent.part.empty();
            const bool shaky = shot.number("shaky", 0u, 0.0f) != 0.0f;

            putU32(out, static_cast<uint32_t>(steps + 1));
            for (int i = 0; i <= steps; ++i)
            {
                const float t = still ? 1.0f : static_cast<float>(i) / static_cast<float>(steps);
                const float frame = from + (to - from) * eased(ease, t);
                const float low = std::min(from, to), high = std::max(from, to);
                const float u = high > low ? (frame - low) / (high - low) : 1.0f;
                const Vec in = shot.vec("offset_in"), offset = in + (shot.vec("offset_out") - in) * u;
                Vec at = position(path, frame) + offset;
                if (turned)
                    at.z *= kSpotDepth;
                const Vec spotIn = shot.vec("singer_in"), spot = spotIn + (shot.vec("singer_out") - spotIn) * u;
                const float fovIn = shot.number("fov_in", 0u, 45.0f);
                float rows[9];
                rotation(path, frame, rows);

                // How long it holds, then how long it takes to the next key
                // and that move's ease, the field of view, where it is and
                // where on screen it holds what it looks at.
                putF32(out, 0.0f);
                putF32(out, i < steps ? frames / static_cast<float>(steps) : 0.0f);
                putF32(out, 0.0f);
                putF32(out, fov(fovIn + (shot.number("fov_out", 0u, 45.0f) - fovIn) * u));
                for (const float v : rows)
                    putF32(out, v);
                putF32(out, at.x);
                putF32(out, at.y);
                putF32(out, at.z);
                putF32(out, spot.x);
                putF32(out, spot.y);
                // Blur before and behind what it looks at, as GH2's shots.
                putF32(out, 0.5f);
                putF32(out, 2.0f);
                putF32(out, 0.9f);
                putU32(out, static_cast<uint32_t>(targets.size()));
                for (const Target &looked : targets)
                    put(out, looked);
                put(out, parent);
                out.push_back(turned ? 1u : 0u);
                // Shake: the noise's size and speed, and the most it turns.
                putF32(out, shaky ? 0.2f : 0.0f);
                putF32(out, shaky ? 0.2f : 0.0f);
                putF32(out, shaky ? 0.25f : 0.0f);
                putF32(out, shaky ? 0.25f : 0.0f);
            }

            out.push_back(0u); // looping
            putF32(out, 0.0f); // the path's ease
            putF32(out, shot.number("near", 0u, 10.0f));
            putF32(out, shot.number("far", 0u, 10000.0f));
            out.push_back(shot.number("enable_dof", 0u, 0.0f) != 0.0f ? 1u : 0u);
            // How fast it follows what it looks at: GH1's cam_filter.
            putF32(out, 0.3f);
            putF32(out, -1.0f); // no height to stay above
            putStr(out, {});    // no path of GH2's
            putF32(out, 0.0f);  // no fade
            putStr(out, category);
            putF32(out, as->weight);
            putU32(out, 0u); // no crowd members picked
            putU32(out, 0xffffffffu);
            putU32(out, 0u); // nothing hidden
            putStr(out, hideCrowd ? "" : "crowd");
            putStr(out, {});
            return out;
        }
    }

    void addCameras(size_t layer, size_t disc, const std::string &gh1, const std::string &gh2, const Stage &stage)
    {
        const dtb::Files files = [disc](const std::string &file) { return ark::readFile(disc, file); };
        dtb::Macros macros;
        const auto script = dtb::read("venues/" + gh1 + "/camera.dta", macros, files);
        const auto camPaths = dtb::read("arena/cam_paths.dta", macros, files);
        const auto scene = load(disc, "venues/" + gh1 + "/gen/campaths.rnd_ps2");
        if (!script || !camPaths || !scene)
        {
            std::cerr << "[gh1] cannot read " << gh1 << "'s cameras" << std::endl;
            return;
        }
        std::map<std::string, Path> paths;
        for (size_t i = 0; i < scene->entries.size(); ++i)
            if (const auto &[cls, name] = scene->entries[i]; cls == "TransAnim" && name.size() > 4u)
                if (auto made = path(scene->bodies[i]))
                    paths[name.substr(0, name.size() - 4u)] = std::move(*made);
        const std::vector<Shot> all = shots(*script);

        // A shot of those groups: the first that stays still if any does and
        // one is wanted, else the first.
        const auto first = [&](const std::set<std::string> &groups, bool still) -> const Shot *
        {
            for (const bool wanted : {still, false})
                for (const Shot &s : all)
                    if (groups.count(s.group) && (!wanted || s.number("start", 0u, 0.0f) == s.number("end", 0u, 100.0f)))
                        return &s;
            return nullptr;
        };

        for (const char *players : {"", "_coop", "_mp"})
        {
            const std::string file = "world/" + gh2 + "/gen/" + gh2 + players + ".milo_ps2";
            const auto world = load(0u, file);
            if (!world)
            {
                std::cerr << "[gh1] cannot read " << file << std::endl;
                continue;
            }
            const bool two = players[0] != '\0';
            milo::Dir out = *world;
            out.entries.clear();
            out.bodies.clear();
            for (size_t i = 0; i < world->entries.size(); ++i)
                if (world->entries[i].first != "CamShot")
                {
                    out.entries.push_back(world->entries[i]);
                    out.bodies.push_back(world->bodies[i]);
                }

            milo::add(out, "Trans", kSpot, trans(stage.spot));

            std::set<std::string> taken;
            size_t made = 0u;
            const auto add = [&](const Shot &shot, const std::string &category, std::string name, bool ended)
            {
                while (!taken.insert(name).second)
                    name += "_";
                if (auto body = camShot(shot, category, ended, two, paths, *camPaths, stage))
                {
                    milo::add(out, "CamShot", name, std::move(*body));
                    ++made;
                }
            };
            // With two players a shot hung under one of them leaves the
            // other out, so only those of the venue's own are kept, where a
            // group has any.
            const auto fixed = [&](const Shot &s) { return !named(*camPaths, s.path, "parent").empty(); };
            std::set<std::string> groupsFixed;
            for (const Shot &s : all)
                if (fixed(s))
                    groupsFixed.insert(s.group);
            for (const Shot &s : all)
            {
                const Kind *as = kind(s.group);
                if (s.group == "FINAL_WIN")
                    add(s, "WIN_GAME", s.name, false);
                else if (as && !(two && !as->special && !fixed(s) && groupsFixed.count(s.group)))
                    add(s, s.group, s.name, false);
            }
            // The categories GH2 asks for that GH1 has no group for, each
            // from the nearest of GH1's: an encore's intro and wins from the
            // intro and win, a retry's intro from where the intro ends, and
            // the lighters' shot from a balcony's.
            if (const Shot *intro = first({"INTRO"}, false))
            {
                add(*intro, "INTRO_ENCORE", intro->name + "_encore", false);
                add(*intro, "INTRO_FAST", intro->name + "_fast", true);
            }
            if (const Shot *win = first({"WIN"}, false))
            {
                add(*win, "WIN_ENCORE", win->name + "_encore", false);
                add(*win, "WIN_ENCORE_SONG", win->name + "_encore_song", false);
                if (!first({"FINAL_WIN"}, false))
                    add(*win, "WIN_GAME", win->name + "_game", false);
            }
            const Shot *far = first({"balcony_lft", "balcony_rt"}, true);
            if (!far)
                far = first({"flr_far_lft", "flr_far_rt"}, true);
            if (far)
                add(*far, "LIGHTER", far->name + "_lighter", false);

            if (made == 0u)
            {
                std::cerr << "[gh1] no cameras for " << gh1 << std::endl;
                continue;
            }
            ark::addFile(layer, file, milo::write(out));
        }
    }
}
