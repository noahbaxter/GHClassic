// A run of taps (gameplay/extended/guitar/gem_kinds.h) joined by a line on
// the track, as later games draw it: one smooth curve through the run, two
// white rails with a gap between. Once a tap is hit its end of the line
// holds a frame, then pulls in to the next tap, across and along at once,
// the curve laid again each frame, so what is left stands up into a short
// hook under the gem.
//
// Which fret joins which is the chart's alone, worked out once as each tap
// is read, in order. Each fret of a tap leaves the run an end, at its tail's
// end if it is held, else at itself. A tap takes the ends the run left by
// the time it starts, the last and any close before it (well sooner ones
// were overtaken by a fret held on past them), and joins them to its frets
// in one go: as few lines as the more of the two, each end and fret on one
// at least, none crossing, moving across least. With none left, all still
// held, each fret branches off the nearest held one where it was struck. A
// line runs some way along at least, so one off a tail ending as the tap
// comes curves out of it. A tap that is not joined (gem_kinds' plain notes,
// or off by the kRope switches) ends the run.
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
        // The look, to tune. Which taps a line joins (GH: Van Halen's only
        // single, unheld ones: both false). A hit tap's end: how long it
        // stays at the now bar, then how long it takes to pull in to the
        // next tap (Van Halen's a frame then about five, 83 ms; slower reads
        // calmer). A held fret's end this close to a tap's start counts as
        // at it; ends this close to a tap all join it, not only the last;
        // and the least a line runs along (a line off a tail leaves it that
        // far short of its end). All times in ms.
        constexpr bool kRopeChords = true, kRopeSustains = true;
        constexpr float kHoldMs = 17.0f, kPullInMs = 140.0f;
        constexpr float kTouchMs = 5.0f, kGatherMs = 150.0f, kMinRunMs = 60.0f;
        // Each rail across a line: its middle's offset and half width.
        constexpr std::array<std::pair<float, float>, 2> kRails{{{-0.24f, 0.1f}, {0.24f, 0.1f}}};
        // How far short of the track's far end a line starts fading out.
        constexpr float kFarFade = 20.0f;

        // Track units, a lane 4 across, and the steps along each curve. How
        // much shorter the camera shows the track along than across, a gem's
        // round top about three times wider than deep. TrackConfig's speed,
        // 0.06 track units a ms at 1 (as lane_bands has it). How many gems on
        // from one added the chart is read for taps: a tap's slope needs the
        // lines out of it, read with the tap after.
        constexpr float kLaneWidth = 4.0f, kFlat = 3.0f, kUnitsPerMs = 0.06f;
        constexpr int kSteps = 24;
        constexpr uint32_t kAhead = 2u, kConfigSpeed = 0x18u;

        // Where a tap's fret leaves a line: the tap (its GameGem), across
        // and along.
        struct End
        {
            uint32_t source;
            float lane, along;
        };
        // A line into a tap, from an end to one of its frets across.
        struct Rope
        {
            End from;
            float toLane;
        };
        // Each tap of the chart read so far, by its GameGem: its track, its
        // place along it, each fret's across and its tail's end (its place
        // if not held; a chord's frets can end apart), its track's speed, its
        // lines in, its Gem while it has one on the track (0 before, and
        // after), and whether it is hit, on the track a frame already (its
        // rep's world good), and gone off it. A missed tap keeps its lines as
        // it scrolls on.
        struct Tap
        {
            uint32_t track;
            float from;
            std::vector<float> lanes, tails;
            float unitsPerMs;
            std::vector<Rope> in;
            uint32_t gem = 0u;
            bool hit = false, settled = false, gone = false;
        };
        std::map<uint32_t, Tap> s_taps;
        std::map<uint32_t, uint32_t> s_onTrack; // each Gem's GameGem
        // Each track's run: its last tap read, and the ends it leaves.
        struct Run
        {
            uint32_t last = 0u;
            std::vector<End> ends;
        };
        std::map<uint32_t, Run> s_runs;
        // A hit tap's end running up, by the tap and its line in: how far
        // the track had scrolled when it went.
        std::map<std::pair<uint32_t, size_t>, float> s_gone;
        uint32_t s_dividersMat = 0u;

        // `a` and `b` (each in order across) paired as few times as the
        // longer has items, each item of both in one pair at least, none
        // crossing, moving across least: two to two pair off, one splits to
        // many. Pairs of indices into a then b.
        std::vector<std::pair<size_t, size_t>> pairUp(const std::vector<float> &a, const std::vector<float> &b)
        {
            const bool flip = a.size() < b.size();
            const std::vector<float> &big = flip ? b : a, &small = flip ? a : b;
            const size_t k = big.size(), s = small.size();
            // Least motion with big's i on small's j, and whether big's
            // i - 1 was on small's j - 1.
            std::vector<float> least(k * s, 1e9f);
            std::vector<bool> stepped(k * s);
            for (size_t i = 0; i < k; ++i)
                for (size_t j = 0; j <= i && j < s; ++j)
                {
                    if (k - i < s - j)
                        continue;
                    const float stay = i == 0 ? 0.0f : j <= i - 1 ? least[(i - 1) * s + j] : 1e9f;
                    const float step = i > 0 && j > 0 ? least[(i - 1) * s + j - 1] : 1e9f;
                    stepped[i * s + j] = step < stay;
                    least[i * s + j] = std::fabs(big[i] - small[j]) + std::min(stay, step);
                }
            std::vector<std::pair<size_t, size_t>> pairs;
            for (size_t i = k, j = s - 1; i-- > 0;)
            {
                pairs.push_back(flip ? std::pair{j, i} : std::pair{i, j});
                if (stepped[i * s + j])
                    --j;
            }
            return pairs;
        }

        // A tap just read, joined to its run's ends (see the top).
        void join(uint32_t game, Tap &tap)
        {
            Run &run = s_runs[tap.track];
            if (run.last + guest::GameGem::kSize != game)
                run.ends.clear();
            run.last = game;
            const float touch = kTouchMs * tap.unitsPerMs;
            std::vector<End> from, held;
            for (const End &end : run.ends)
                (end.along <= tap.from + touch ? from : held).push_back(end);
            // Of the ends left, the last to come, and any within kGatherMs
            // of this tap: one that came well before was taken over by a
            // fret held on past it.
            float latest = -1e9f;
            for (const End &end : from)
                latest = std::max(latest, end.along);
            const float keep = std::min(latest, tap.from - kGatherMs * tap.unitsPerMs) - touch;
            std::erase_if(from, [&](const End &end) { return end.along < keep; });
            std::sort(from.begin(), from.end(), [](const End &a, const End &b) { return a.lane < b.lane; });
            std::vector<float> lanes;
            for (const End &end : from)
                lanes.push_back(end.lane);
            if (!from.empty())
                for (const auto &[i, j] : pairUp(lanes, tap.lanes))
                    tap.in.push_back({from[i], tap.lanes[j]});
            else
                // None left, all still held: each fret off the held one
                // nearest it, from where it was struck.
                for (const float lane : tap.lanes)
                    if (const auto near = std::min_element(held.begin(), held.end(), [lane](const End &a, const End &b)
                                                           { return std::fabs(a.lane - lane) < std::fabs(b.lane - lane); });
                        near != held.end())
                        tap.in.push_back({{near->source, near->lane, s_taps.at(near->source).from}, lane});
            // Each line kMinRunMs along at least: one off a held fret leaves
            // its tail that far short of its end, so it curves out of it, not
            // across the track where the tail ends as this tap comes.
            for (Rope &rope : tap.in)
                rope.from.along = std::max(s_taps.at(rope.from.source).from, std::min(rope.from.along, tap.from - kMinRunMs * tap.unitsPerMs));
            run.ends = held;
            for (size_t i = 0; i < tap.lanes.size(); ++i)
                run.ends.push_back({game, tap.lanes[i], tap.tails[i]});
        }

        // A tap's slope, across over along: through a single, unheld tap
        // with at most one line in and one out (only ever to the tap after
        // it), from where the one in leaves to where the one out goes, so the
        // run is one smooth curve; else 0, its lines leaving and reaching it
        // straight along.
        float slope(uint32_t game)
        {
            const Tap &tap = s_taps.at(game);
            if (tap.lanes.size() != 1u || tap.tails[0] != tap.from || tap.in.size() > 1u)
                return 0.0f;
            float ax = tap.lanes[0], ay = tap.from, bx = ax, by = ay;
            if (!tap.in.empty())
                ax = tap.in[0].from.lane, ay = tap.in[0].from.along;
            if (const auto next = s_taps.find(game + guest::GameGem::kSize); next != s_taps.end())
            {
                const auto &in = next->second.in;
                const auto ours = [game](const Rope &r) { return r.from.source == game; };
                const auto out = std::count_if(in.begin(), in.end(), ours);
                if (out > 1)
                    return 0.0f;
                if (out == 1)
                    bx = std::find_if(in.begin(), in.end(), ours)->toLane, by = next->second.from;
            }
            return by > ay ? (bx - ax) / (by - ay) : 0.0f;
        }

        // The GameGem at `game`, read as a tap of the track's chart if it is
        // one past `after`'s tick, and whether it is one.
        bool readTap(uint8_t *rdram, uint32_t game, uint32_t track, float unitsPerMs, int32_t after)
        {
            if (s_taps.count(game))
                return true;
            const guest::GameGem gem(rdram, game);
            const uint8_t slots = gem.slots() & 0x1fu;
            if (gem.tick() <= after || !isTap(track, static_cast<uint32_t>(gem.tick()), gem.slots()) ||
                (!kRopeChords && (slots & (slots - 1u)) != 0u) ||
                (!kRopeSustains && !gem.noSustain() && gem.lengthMs() > 0u))
                return false;
            const float held = !gem.noSustain() ? float(gem.lengthMs()) : 0.0f;
            // Each fret's place across, an open's the middle lane's, and its
            // tail's end: a chord whose frets end apart keeps each one's.
            const Gh2Gem *chart = chartGem(track, static_cast<uint32_t>(gem.tick()), gem.slots());
            const bool split = held > 0.0f && chart && chart->split();
            std::vector<float> lanes, tails;
            for (int fret = 0; fret < 5; ++fret)
                if ((gem.slots() >> fret) & 1u)
                {
                    lanes.push_back(float(fret - 2) * kLaneWidth);
                    tails.push_back((gem.ms() + (split ? chart->laneMs[size_t(fret)] : held)) * unitsPerMs);
                }
            if (lanes.empty())
            {
                lanes.push_back(0.0f);
                tails.push_back((gem.ms() + held) * unitsPerMs);
            }
            Tap &tap = s_taps[game] = {track, gem.ms() * unitsPerMs, lanes, tails, unitsPerMs};
            join(game, tap);
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
            // A longer highway draws the dividers out along by as much
            // (note_speed.cpp's onSetupFade), and the line with them unless
            // undone, so it would run on ahead of the gems.
            const float along = load<float>(rdram, world + 0x14u);
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
                        // Its width taken as the camera sees the track, along
                        // it kFlat times shorter, so a line running across
                        // is as thick as one running along, not a hairline.
                        const float tx = bx - ax, ty = (by - ay) / kFlat, len = std::max(1e-5f, std::hypot(tx, ty));
                        for (const float side : {-1.0f, 1.0f})
                        {
                            const float across = offset + half * side;
                            Vertex v = base;
                            v.pos[0] = x - ty / len * across - ox;
                            v.pos[1] = (y + tx / len * across * kFlat - oy) / along;
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
                const auto place = places.find(tap.track);
                if (tap.hit || tap.gone || place == places.end())
                    continue;
                const float tx = place->second[0], ty = place->second[1];
                // Each line in, while the tap it leaves is on the track or
                // its end is pulling in: once that is hit and its end has
                // come down to the now bar, the end held there, not
                // scrolling on under it, then pulled in from there to this
                // tap at an even pace, easing only at its ends.
                for (size_t r = 0; r < tap.in.size(); ++r)
                {
                    const Rope &rope = tap.in[r];
                    const Tap &source = s_taps.at(rope.from.source);
                    if (source.gem == 0u && !source.hit)
                        continue;
                    float in = 0.0f, from = rope.from.along;
                    if (source.hit && ty + from <= 0.0f)
                    {
                        const auto gone = s_gone.try_emplace({game, r}, -ty).first;
                        const float scrolled = -ty - gone->second;
                        const float t = std::clamp((scrolled / tap.unitsPerMs - kHoldMs) / kPullInMs, 0.0f, 1.0f);
                        if (t >= 1.0f)
                            continue;
                        in = t * t * (3.0f - 2.0f * t);
                        from += scrolled;
                    }
                    const float gy = ty + tap.from, py = ty + from + (tap.from - from) * in;
                    const float gm = slope(game), pm = slope(rope.from.source);
                    ribbon(tx + rope.from.lane + (rope.toLane - rope.from.lane) * in, py, pm + (gm - pm) * in, tx + rope.toLane, gy, gm);
                }
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
            s_taps.clear(), s_onTrack.clear(), s_runs.clear(), s_gone.clear();
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
