// A run of taps (gameplay/extended/guitar/gem_kinds.h) joined by a line on
// the track, as later games draw it: one smooth curve through the run, a
// trail for each fret of a chord, two white rails with a gap between. Once
// a tap is hit its end of the line holds a frame, then pulls in to the next
// tap, across and along at once, the curve laid again each frame, so what is
// left stands up into a short hook under the gem.
//
// The line is the chart's, not the gems': each tap is its GameGem (SongDB's,
// a track's in one array, 0x10 each), placed along the track at its ms by the
// track's speed, so the line runs on past the last gem added, up to the next
// taps, and fades with the far end as gems do (the track's environ fades
// what blends, note_speed.cpp).
//
// Drawn with the track's lane dividers (a mesh rider), which lie still, so
// under the gems. A gem's group sits at its place along the track (its
// local y) in a parent that scrolls: where the track is now is a gem's
// world less its local, the dividers' frame their world less theirs.

#include "highway/extended/guitar/tap_line.h"

#include "gameplay/extended/guitar/gem_kinds.h"
#include "guest.h"
#include "guest/beatmatch.h"
#include "highway/gem_looks.h"
#include "hook.h"
#include "milo/layout.h"
#include "render/native_mesh.h"
#include "settings/settings.h"

#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <utility>
#include <vector>

namespace gh2::guitar
{
    namespace
    {
        // A rep's group (its first word): its RndTransformable at +0x50.
        constexpr uint32_t kGroupXfm = 0x50u;
        constexpr const char *kDividersMat = "division_lines.mat";
        // Track units, a lane 4 across. The steps along each curve, and a
        // hit tap's end: how long it stays at the now bar, then how long it
        // takes to pull in to the next tap (GH: Van Halen's, a frame then
        // about five). How many gems on from one added the chart is read
        // for taps: the next's slope needs the one after it. TrackConfig:
        // its speed, 0.06 track units a ms at 1 (as
        // highway/extended/guitar/lane_bands has it).
        constexpr float kLaneWidth = 4.0f;
        constexpr int kSteps = 24;
        constexpr float kHoldMs = 17.0f, kPullInMs = 83.0f, kUnitsPerMs = 0.06f;
        // How far short of the track's far end a line starts fading out.
        constexpr float kFarFade = 20.0f;
        constexpr uint32_t kAhead = 2u, kConfigSpeed = 0x18u;

        // Each rail across a line: its middle's offset and half width.
        constexpr std::array<std::pair<float, float>, 2> kRails{{{-0.24f, 0.1f}, {0.24f, 0.1f}}};

        // Each tap of the chart read so far, by its GameGem: its track, its
        // place along it and each fret's across, its track's speed, its Gem
        // while it has one on the track (0 before, and after), and whether
        // it is hit, on the track a frame already (its rep's world good),
        // and gone off it. A missed tap keeps its line as it scrolls on.
        struct Tap
        {
            uint32_t track;
            float from;
            std::vector<float> lanes;
            float unitsPerMs;
            uint32_t gem = 0u;
            bool hit = false, settled = false, gone = false;
        };
        std::map<uint32_t, Tap> s_taps;
        std::map<uint32_t, uint32_t> s_onTrack; // each Gem's GameGem
        // A hit tap's end running up, by the tap after it: how far the
        // track had scrolled when it went.
        std::map<uint32_t, float> s_gone;
        uint32_t s_dividersMat = 0u;

        // The GameGem at `game`, read as a tap of the track's chart if it is
        // one past `after`'s tick, and whether it is one.
        bool readTap(uint8_t *rdram, uint32_t game, uint32_t track, float unitsPerMs, int32_t after)
        {
            if (s_taps.count(game))
                return true;
            const guest::GameGem gem(rdram, game);
            if (gem.tick() <= after || !isTap(track, static_cast<uint32_t>(gem.tick()), gem.slots()))
                return false;
            // Each fret's place across, an open's the middle lane's.
            std::vector<float> lanes;
            for (int fret = 0; fret < 5; ++fret)
                if ((gem.slots() >> fret) & 1u)
                    lanes.push_back(float(fret - 2) * kLaneWidth);
            if (lanes.empty())
                lanes.push_back(0.0f);
            s_taps[game] = {track, gem.ms() * unitsPerMs, lanes, unitsPerMs};
            return true;
        }

        void gemAdded(uint8_t *rdram, R5900Context *, PS2Runtime *, const GemAdded &added)
        {
            // The pool's Gem taken for a new one: the tap it was is gone.
            if (const auto was = s_onTrack.find(added.gem); was != s_onTrack.end())
            {
                if (const auto tap = s_taps.find(was->second); tap != s_taps.end())
                    tap->second.gem = 0u, tap->second.gone = true;
                s_onTrack.erase(was);
            }
            if (!playingCustom() || added.game == 0u)
                return;
            const uint32_t config = load<uint32_t>(rdram, added.manager + gemManager::kConfig);
            const float unitsPerMs = kUnitsPerMs * load<float>(rdram, config + kConfigSpeed);
            const int32_t tick = guest::GameGem(rdram, added.game).tick();
            if (readTap(rdram, added.game, added.track, unitsPerMs, tick - 1))
            {
                Tap &tap = s_taps.at(added.game);
                tap.gem = added.gem, tap.settled = false;
                s_onTrack[added.gem] = added.game;
            }
            // The run on from it, ahead of the track's end.
            int32_t after = tick;
            for (uint32_t k = 1u; k <= kAhead; ++k)
            {
                const uint32_t game = added.game + k * guest::GameGem::kSize;
                if (!readTap(rdram, game, added.track, unitsPerMs, after))
                    break;
                after = guest::GameGem(rdram, game).tick();
            }
        }

        // Where a gem's track lies in the world, its scroll and its place
        // across, from its first rep, if it has one on the track.
        bool trackPlace(uint8_t *rdram, uint32_t gem, float &x, float &y)
        {
            const uint32_t r = load<uint32_t>(rdram, gem + gem::kReps);
            if (r == load<uint32_t>(rdram, gem + gem::kRepsEnd))
                return false;
            const uint32_t group = load<uint32_t>(rdram, load<uint32_t>(rdram, r)) + kGroupXfm;
            const uint32_t local = group + ::milo::transformable::kLocal, world = group + ::milo::transformable::kWorld;
            x = load<float>(rdram, world + 0x30u) - load<float>(rdram, local + 0x30u);
            y = load<float>(rdram, world + 0x34u) - load<float>(rdram, local + 0x34u);
            return true;
        }

        void ride(uint8_t *rdram, uint32_t mesh, const MeshData &geometry, std::vector<std::shared_ptr<const MeshData>> &out)
        {
            if (s_taps.empty() || geometry.verts.empty())
                return;
            const uint32_t mat = load<uint32_t>(rdram, mesh + ::milo::mesh::kMat);
            if (s_dividersMat == 0u && mat != 0u && objectName(rdram, mat) == kDividersMat)
                s_dividersMat = mat;
            if (mat == 0u || mat != s_dividersMat)
                return;
            const uint32_t world = mesh + ::milo::mesh::kTransform + ::milo::transformable::kWorld;
            const float ox = load<float>(rdram, world + 0x30u), oy = load<float>(rdram, world + 0x34u);
            auto line = std::make_shared<MeshData>();
            line->transient = true;
            const Vertex base = geometry.verts.front();
            // The track's far end, where gems are added (the now bar at 0):
            // a line is cut there, fading out over the stretch before it.
            const float far = float(settings::get(settings::kHighwayLength));
            // The line from p to g, each with its slope across over along: a
            // Hermite curve, so a run's segments join as one smooth curve.
            const auto ribbon = [&](float px, float py, float pm, float gx, float gy, float gm)
            {
                const float dy = gy - py;
                const auto at = [&](float u, float &x, float &y)
                {
                    const float u2 = u * u, u3 = u2 * u;
                    x = (2.0f * u3 - 3.0f * u2 + 1.0f) * px + (u3 - 2.0f * u2 + u) * pm * dy + (3.0f * u2 - 2.0f * u3) * gx +
                        (u3 - u2) * gm * dy;
                    y = py + dy * u;
                };
                if (py >= far)
                    return;
                const float last = gy > far ? (far - py) / dy : 1.0f;
                for (const auto &[offset, half] : kRails)
                {
                    const uint16_t first = uint16_t(line->verts.size());
                    for (int i = 0; i <= kSteps; ++i)
                    {
                        const float u = last * float(i) / float(kSteps);
                        float x, y, ax, ay, bx, by;
                        at(u, x, y);
                        at(std::max(0.0f, u - 0.01f), ax, ay);
                        at(std::min(1.0f, u + 0.01f), bx, by);
                        const float tx = bx - ax, ty = by - ay, len = std::max(1e-5f, std::hypot(tx, ty));
                        for (const float side : {-1.0f, 1.0f})
                        {
                            const float across = offset + half * side;
                            Vertex v = base;
                            v.pos[0] = x - ty / len * across - ox;
                            v.pos[1] = y + tx / len * across - oy;
                            v.pos[2] = 0.0f;
                            v.color[0] = v.color[1] = v.color[2] = std::clamp((far - y) / kFarFade, 0.0f, 1.0f);
                            v.color[3] = 1.0f;
                            line->verts.push_back(v);
                        }
                    }
                    for (uint16_t i = 0; i < kSteps; ++i)
                    {
                        const uint16_t a = uint16_t(first + i * 2), b = uint16_t(a + 1), c = uint16_t(a + 2), d = uint16_t(a + 3);
                        for (const uint16_t k : {a, b, c, b, d, c})
                            line->indices.push_back(k);
                    }
                }
            };
            // A tap's neighbours in its run, the GameGems either side of it,
            // and its slope: across over along between them (a chord's
            // middle), or to its one neighbour at a run's end; a chord's own
            // lines leave it straight.
            const auto neighbour = [&](uint32_t game, int side) -> const Tap *
            {
                const auto found = s_taps.find(game + uint32_t(side) * guest::GameGem::kSize);
                return found != s_taps.end() && found->second.track == s_taps.at(game).track ? &found->second : nullptr;
            };
            const auto slope = [&](uint32_t game, const Tap &tap)
            {
                const Tap *p = neighbour(game, -1), *n = neighbour(game, 1);
                const Tap &a = p ? *p : tap, &b = n ? *n : tap;
                const auto middle = [](const Tap &t) { return (t.lanes.front() + t.lanes.back()) * 0.5f; };
                return tap.lanes.size() == 1u && b.from > a.from ? (middle(b) - middle(a)) / (b.from - a.from) : 0.0f;
            };
            // Each track's place, the middle of its unhit taps' that were on
            // it last frame: a rep the pool has just lent still has its last
            // gem's world for a frame, and a hit gem's is held at the now bar.
            std::map<uint32_t, std::vector<std::array<float, 2>>> seen;
            for (auto &[game, tap] : s_taps)
                if (float x, y; tap.gem != 0u && trackPlace(rdram, tap.gem, x, y))
                {
                    if (tap.settled && !tap.hit)
                        seen[tap.track].push_back({x, y});
                    tap.settled = true;
                }
            std::map<uint32_t, std::array<float, 2>> places;
            for (auto &[track, all] : seen)
            {
                const auto middle = all.begin() + std::ptrdiff_t(all.size() / 2u);
                std::nth_element(all.begin(), middle, all.end(), [](const auto &a, const auto &b) { return a[1] < b[1]; });
                places[track] = *middle;
            }
            for (const auto &[game, tap] : s_taps)
            {
                // To each tap not hit nor gone from the one before it, while
                // that one is on the track or its end is pulling in.
                const Tap *prev = neighbour(game, -1);
                const auto place = places.find(tap.track);
                if (!prev || tap.hit || tap.gone || place == places.end() || (prev->gem == 0u && !prev->hit))
                    continue;
                const float tx = place->second[0], ty = place->second[1];
                // A hit tap's end held where it was hit, at the now bar, not
                // scrolling on under it, then pulled in from there to this
                // one at an even pace, easing only at its ends.
                float in = 0.0f, from = prev->from;
                if (prev->hit)
                {
                    const auto gone = s_gone.try_emplace(game, -ty).first;
                    const float scrolled = -ty - gone->second;
                    const float t = std::clamp((scrolled / tap.unitsPerMs - kHoldMs) / kPullInMs, 0.0f, 1.0f);
                    if (t >= 1.0f)
                        continue;
                    in = t * t * (3.0f - 2.0f * t);
                    from += scrolled;
                }
                // A line for each of this tap's frets from the nearest of the
                // last tap's, and from each of the last's left over to the
                // nearest of this one's: a chord splits off a single tap, and
                // joins back into one.
                const std::vector<float> &to = tap.lanes, &was = prev->lanes;
                const auto nearest = [](const std::vector<float> &in, float x)
                { return *std::min_element(in.begin(), in.end(), [x](float a, float b) { return std::fabs(a - x) < std::fabs(b - x); }); };
                std::vector<std::pair<float, float>> pairs;
                for (const float g : to)
                    pairs.push_back({nearest(was, g), g});
                for (const float p : was)
                    if (std::none_of(pairs.begin(), pairs.end(), [p](const auto &pair) { return pair.first == p; }))
                        pairs.push_back({p, nearest(to, p)});
                const float gy = ty + tap.from, py = ty + from + (tap.from - from) * in;
                const float gm = slope(game, tap), pm = slope(game - guest::GameGem::kSize, *prev);
                for (const auto &[p, g] : pairs)
                    ribbon(tx + p + (g - p) * in, py, pm + (gm - pm) * in, tx + g, gy, gm);
            }
            if (!line->indices.empty())
                out.push_back(std::move(line));
        }

        // Gem::Hit.
        struct HitTag;
        void onHit(uint8_t *, R5900Context *ctx, PS2Runtime *)
        {
            if (const auto game = s_onTrack.find(GPR_U32(ctx, 4)); game != s_onTrack.end())
                if (const auto tap = s_taps.find(game->second); tap != s_taps.end())
                    tap->second.hit = true;
        }

        void trackGone(uint8_t *, R5900Context *, PS2Runtime *, uint32_t)
        {
            s_taps.clear(), s_onTrack.clear(), s_gone.clear();
            s_dividersMat = 0u;
        }
    }

    void installTapLine(PS2Runtime &runtime, const Addresses &addresses)
    {
        onGemAdded(gemAdded);
        EntryHook<HitTag>::install(runtime, addresses.gemHit, onHit);
        onTrackGone(trackGone);
        addMeshRider(ride);
    }
}
