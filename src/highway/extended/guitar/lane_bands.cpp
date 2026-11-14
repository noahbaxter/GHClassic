// A lane's band is a GemRep's tail (CreateTail, 0x159b40), which
// GemRep::EnableDuration lends a rep whose Gem runs on (its place +0x10
// short of its end +0x14, as GemManager::SetupGems sets them from its
// GameGem). Gem::Poll draws it from +0x18 along to the end, and GemRep::Poll
// writes its rows each frame (six vertices across, a tube), synced by
// PsMesh::Sync. A Gem once hit is held by PollHitGems (0x156938), its tail
// eaten at the now bar, unless it is let go (+0x1c's 0x400).

#include "highway/extended/guitar/lane_bands.h"

#include "custom/gh2_chart.h"
#include "gameplay/extended/guitar/gem_kinds.h"
#include "highway/gem_looks.h"
#include "guest/beatmatch.h"
#include "milo/layout.h"
#include "guest.h"
#include "hook.h"

#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <iterator>
#include <map>

namespace gh2::guitar
{
    namespace
    {
        const Addresses *s_addresses = nullptr;

        // Gem: its place along the track, its end, where its tail starts,
        // its flags (0x400 let go). TrackConfig: its speed, 0.06 track units
        // a ms at 1 (GemSpacing, 0x152490).
        constexpr uint32_t kGemFrom = 0x10u, kGemTo = 0x14u, kGemTailStart = 0x18u;
        constexpr uint32_t kGemLetGo = 0x400u;
        constexpr uint32_t kConfigSpeed = 0x18u;
        constexpr float kUnitsPerMs = 0.06f;
        // A rep's glow (a RndLine): shown at +0x08. Its group (the rep's
        // first word): its place across, its RndTransformable at +0x50.
        constexpr uint32_t kLineShowing = 0x08u;
        constexpr uint32_t kGroupLocalX = 0x50u + ::milo::transformable::kLocal + 0x30u;
        // A tail's place along the track from its gem (its local y).
        constexpr uint32_t kTailLocalY = ::milo::mesh::kTransform + ::milo::transformable::kLocal + 0x34u;

        // How a band looks, in track units (a lane 4 across). Half its
        // width from its fret's middle, short of the dividers. Before and
        // after: how far it runs before its first note and past its last.
        // Cap: how long each round end is. Strength: how much of its colour
        // is added to the track's. It lies in its gems' plane: drawn with
        // no depth it needs no lift to show, and any would draw it further
        // along the track the further off it is, as the camera looks down
        // on it.
        // An open's, from its rep on the middle lane, across all five.
        constexpr float kLaneWidth = 4.0f, kBandHalf = 1.9f, kOpenBandHalf = 2.0f * kLaneWidth + kBandHalf;
        constexpr float kBandBefore = 5.0f, kBandAfter = 5.5f, kBandCap = 2.5f, kBandStrength = 0.4f;
        // Each end's rows, and the colour by fret, an open's purple last.
        constexpr uint32_t kBandCapRows = 8u;
        constexpr int kOpenBand = 5;
        constexpr float kBandColors[6][3] = {{0.1f, 1.0f, 0.2f},  {1.0f, 0.1f, 0.1f}, {1.0f, 0.9f, 0.1f},
                                             {0.15f, 0.4f, 1.0f}, {1.0f, 0.5f, 0.0f}, {0.6f, 0.2f, 1.0f}};
        // Its material: the track scene's tail_white (coloured by its
        // vertices), copied as one of its own (open sustains draw on
        // tail_white), made additive and depthless once found.
        constexpr const char *kWhiteMat = "tail_white.mat", *kBandMat = "lane_band.mat";
        void addBandMat(milo::Dir &scene)
        {
            addCopy(scene, kWhiteMat, kBandMat, [](milo::Bytes &) { return true; });
        }

        // Each band gem on a track: its frets (its reps' in order) and
        // those with bands, an open's (no fret, one rep) its own bit, and
        // its track.
        struct BandGem
        {
            uint8_t frets, banded;
            int gap;
            uint32_t track;
        };
        std::map<uint32_t, BandGem> s_bandGems;
        // Each band's tail, by the this its Sync takes: its rep, its mesh,
        // its fret and, an open's, the fret lane it leaves out (-1 none),
        // and its track.
        struct Band
        {
            uint32_t rep, tail;
            int fret, gap;
            uint32_t track;
        };
        std::map<uint32_t, Band> s_bands;
        // Every track's gems and lane dividers are of the one track scene,
        // its material and theirs the same for each.
        uint32_t s_bandMat = 0u;
        // The track's lane dividers, the first it draws over its surface.
        constexpr const char *kDividersMat = "division_lines.mat";
        uint32_t s_dividersMat = 0u;
        // Track: its TrackConfig. The track drawing now (Track::Draw), -1
        // for none.
        constexpr uint32_t kTrackConfig = 0x28u;
        int64_t s_drawingTrack = -1;

        // A gem added to the track: a band's first, run on to the band's
        // end.
        void gemAdded(uint8_t *rdram, R5900Context *, PS2Runtime *, const GemAdded &added)
        {
            s_bandGems.erase(added.gem);
            if (!playingCustom() || !added.game)
                return;
            const guest::GameGem game(rdram, added.game);
            const Gh2Gem *chart = chartGem(added.track, static_cast<uint32_t>(game.tick()), game.slots());
            if (!chart)
                return;
            uint8_t banded = 0u;
            float longest = 0.0f;
            for (int fret = 0; fret < 5; ++fret)
                if ((game.slots() >> fret) & 1u && chart->bandMs[size_t(fret)] > 0.0f)
                {
                    banded |= static_cast<uint8_t>(1u << fret);
                    longest = std::max(longest, chart->bandMs[size_t(fret)]);
                }
            const bool open = chart->open || chart->openChord;
            if (open && chart->bandMs[kOpenBand] > 0.0f)
                banded |= 1u << kOpenBand, longest = std::max(longest, chart->bandMs[kOpenBand]);
            if (banded == 0u)
                return;
            const uint32_t config = load<uint32_t>(rdram, added.manager + gemManager::kConfig);
            const float spacing = load<float>(rdram, config + kConfigSpeed) * kUnitsPerMs;
            const float from = load<float>(rdram, added.gem + kGemFrom);
            store<float>(rdram, added.gem + kGemTo, from + longest * spacing + kBandAfter);
            store<float>(rdram, added.gem + kGemTailStart, -kBandBefore);
            s_bandGems[added.gem] = {game.slots(), banded, open ? int(chart->openBandGap) : -1, added.track};
        }

        // GemRep::EnableDuration, which lends the rep a tail: a band's, its
        // material additive.
        PS2Runtime::RecompiledFunction s_enableDuration = nullptr;
        void enableDuration(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t rep = GPR_U32(ctx, 4);
            callOriginal(rdram, ctx, runtime, s_addresses->gemRepEnableDuration, s_enableDuration);
            const uint32_t tail = load<uint32_t>(rdram, rep + gemRep::kTail);
            if (tail == 0u || s_bandGems.empty())
                return;
            for (const auto &[gem, band] : s_bandGems)
            {
                // Its reps a fret each, lowest first, then an open's
                // (an open's its one, highway/extended/guitar/open_gems).
                uint8_t left = band.frets | (band.banded & uint8_t(1u << kOpenBand));
                for (uint32_t r = load<uint32_t>(rdram, gem + gem::kReps), last = load<uint32_t>(rdram, gem + gem::kRepsEnd);
                     r < last && left; r += 4u, left &= static_cast<uint8_t>(left - 1u))
                {
                    if (load<uint32_t>(rdram, r) != rep)
                        continue;
                    const int fret = std::countr_zero(left);
                    if (!((band.banded >> fret) & 1u))
                        return;
                    s_bands[syncThis(rdram, tail)] = {rep, tail, fret, band.gap, band.track};
                    if (s_bandMat == 0u)
                    {
                        const uint32_t cap = partObject(rdram, rep, gemRep::kCap);
                        const uint32_t scene = cap ? load<uint32_t>(rdram, cap + ::milo::object::kDir) : 0u;
                        s_bandMat = scene ? findObject(rdram, ctx, runtime, scene, kBandMat) : 0u;
                        s_dividersMat = scene ? findObject(rdram, ctx, runtime, scene, kDividersMat) : 0u;
                        if (s_bandMat != 0u)
                        {
                            store<uint32_t>(rdram, s_bandMat + ::milo::mat::kBlend, ::milo::mat::kBlendAdd);
                            store<uint32_t>(rdram, s_bandMat + ::milo::mat::kZMode, ::milo::mat::kZDisable);
                        }
                    }
                    return;
                }
            }
        }

        // Gem::Hit: a band's Gem let go at once, so its tail scrolls on
        // under the now bar, unheld, and its glow hidden.
        PS2Runtime::RecompiledFunction s_gemHit = nullptr;
        void gemHit(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t gem = GPR_U32(ctx, 4);
            callOriginal(rdram, ctx, runtime, s_addresses->gemHit, s_gemHit);
            if (!s_bandGems.count(gem))
                return;
            store<uint32_t>(rdram, gem + gem::kFrets, load<uint32_t>(rdram, gem + gem::kFrets) | kGemLetGo);
            for (uint32_t r = load<uint32_t>(rdram, gem + gem::kReps), last = load<uint32_t>(rdram, gem + gem::kRepsEnd);
                 r < last; r += 4u)
                if (const uint32_t line = load<uint32_t>(rdram, load<uint32_t>(rdram, r) + gemRep::kGlow))
                    store<uint32_t>(rdram, line + kLineShowing, 0u);
        }

        // PsMesh::Sync(int): a band's tail laid flat, a lane wide, in its
        // colour, before it is taken; its own material again if a miss
        // greyed it.
        struct TailSyncTag;
        void onTailSync(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const EntryArgs args(ctx);
            const auto found = s_bands.find(args.a[0]);
            if (found == s_bands.end())
                return;
            const Band &band = found->second;
            if (load<uint32_t>(rdram, band.rep + gemRep::kTail) != band.tail)
            {
                s_bands.erase(found);
                return;
            }
            if (s_bandMat != 0u && load<uint32_t>(rdram, band.tail + ::milo::mesh::kMat) != s_bandMat)
            {
                runtime->callGuestFunction(rdram, ctx, s_addresses->rndMeshSetMat, {band.tail, s_bandMat});
                args.restore(ctx);
            }
            const uint32_t verts = load<uint32_t>(rdram, band.tail + ::milo::mesh::kVerts);
            const int32_t count = load<int32_t>(rdram, band.tail + ::milo::mesh::kVertCount);
            if (verts == 0u || count < int32_t(gemRep::kTailAcross))
                return;
            const float *rgb = kBandColors[band.fret];
            const float half = band.fret == kOpenBand ? kOpenBandHalf : kBandHalf;
            // Each vertex's place across, edge to edge; an open's beside a
            // fret's leaves that lane out, its two middle vertices unlit
            // from one edge of the lane to the other, so the fret's band
            // shows there in its own colour, not washed purple.
            float xs[gemRep::kTailAcross];
            for (uint32_t j = 0; j < gemRep::kTailAcross; ++j)
                xs[j] = (float(j) / float(gemRep::kTailAcross - 1u) * 2.0f - 1.0f) * half;
            if (band.gap >= 0)
            {
                const float lane = float(band.gap - 2) * kLaneWidth;
                const float from = std::max(-half, lane - kLaneWidth * 0.5f), to = std::min(half, lane + kLaneWidth * 0.5f);
                const float gapped[6] = {-half, from, from, to, to, half};
                std::copy(std::begin(gapped), std::end(gapped), xs);
            }
            // Every vertex on the texel the middle of its first row has, so
            // the band is one colour, not the tube's shading across.
            float uv[2];
            std::memcpy(uv, getMemPtr(rdram, verts + 2u * ::milo::mesh::kVertSize + ::milo::mesh::kVertUv), sizeof uv);
            // Its rows laid again from Poll's first to its last: a half
            // ellipse round each end (its first row and its last the tips),
            // the rows between evenly along. Ends cut short by the track's
            // near or far end are capped there.
            const uint32_t rows = uint32_t(count) / gemRep::kTailAcross;
            const auto rowY = [&](uint32_t row)
            {
                return load<float>(rdram, verts + row * gemRep::kTailAcross * ::milo::mesh::kVertSize +
                                              ::milo::mesh::kVertPos + 4u);
            };
            // The tail is placed at its start along from its gem (its local
            // y), which Gem::Poll holds at the now bar once its gem has passed
            // it: its front laid where the band starts, `before` short of
            // its first note, wherever that is; its back where Poll ends it
            // (the band's end, or the track's far end).
            const float start = load<float>(rdram, band.tail + kTailLocalY);
            const float first = -kBandBefore - start, last = rowY(rows - 1u);
            const uint32_t capRows = std::min(kBandCapRows, rows / 2u);
            const float cap = std::min(kBandCap, (last - first) * 0.5f);
            for (int32_t i = 0; i < count; ++i)
            {
                const uint32_t v = verts + uint32_t(i) * ::milo::mesh::kVertSize;
                const uint32_t row = uint32_t(i) / gemRep::kTailAcross;
                const uint32_t across = uint32_t(i) % gemRep::kTailAcross;
                const bool unlit = band.gap >= 0 && (across == 2u || across == 3u);
                float y, narrow = 1.0f;
                // In a cap, its angle round from the tip (0) to the full
                // width (a quarter turn).
                const bool front = capRows >= 2u && row < capRows, back = capRows >= 2u && row >= rows - capRows;
                if (front || back)
                {
                    const float angle = 1.5707964f * float(front ? row : rows - 1u - row) / float(capRows - 1u);
                    const float in = cap * (1.0f - std::cos(angle));
                    y = front ? first + in : last - in;
                    narrow = std::sin(angle);
                }
                else
                {
                    const float along = float(row - capRows + 1u) / float(rows - 2u * capRows + 1u);
                    y = first + cap + (last - first - 2.0f * cap) * along;
                }
                store<float>(rdram, v + ::milo::mesh::kVertPos, xs[across] * narrow);
                store<float>(rdram, v + ::milo::mesh::kVertPos + 4u, y);
                store<float>(rdram, v + ::milo::mesh::kVertPos + 8u, 0.0f);
                for (uint32_t c = 0; c < 3u; ++c)
                    store<float>(rdram, v + ::milo::mesh::kVertColor + c * 4u, unlit ? 0.0f : rgb[c] * kBandStrength);
                store<float>(rdram, v + ::milo::mesh::kVertColor + 12u, 1.0f);
                std::memcpy(getMemPtr(rdram, v + ::milo::mesh::kVertUv), uv, sizeof uv);
            }
        }

        // Track::Draw, each player's track in turn, with its own camera.
        PS2Runtime::RecompiledFunction s_trackDraw = nullptr;
        void trackDraw(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const EntryArgs args(ctx);
            s_drawingTrack = trackNum(rdram, ctx, runtime, args.a[0] + kTrackConfig);
            args.restore(ctx);
            callOriginal(rdram, ctx, runtime, s_addresses->trackDraw, s_trackDraw);
            s_drawingTrack = -1;
        }

        // PsMesh::DrawShowing: the bands drawn on the track's surface, under
        // everything on it. The track draws its surface, then its lane
        // dividers (kDividersMat), beat lines and rails, then its gems'
        // tails and gems: the bands go in before the dividers, not where
        // their tails would draw. Added, and neither tested against depth
        // nor writing their own, so the dividers, lines, rails, now bar and
        // gems all keep theirs. Each track's dividers are a mesh of their
        // own on the one material: its bands go in with its own.
        PS2Runtime::RecompiledFunction s_meshDraw = nullptr;
        bool s_drawingBands = false;
        void meshDraw(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t mesh = GPR_U32(ctx, 4);
            if (!s_bands.empty() && !s_drawingBands)
            {
                for (const auto &[self, band] : s_bands)
                    if (band.tail == mesh)
                        return leave(ctx);
                if (s_dividersMat != 0u && load<uint32_t>(rdram, mesh + ::milo::mesh::kMat) == s_dividersMat)
                {
                    const EntryArgs args(ctx);
                    s_drawingBands = true;
                    for (const auto &[self, band] : s_bands)
                        if (band.track == s_drawingTrack && load<uint32_t>(rdram, band.rep + gemRep::kTail) == band.tail)
                            runtime->callGuestFunction(rdram, ctx, s_addresses->psMeshDrawShowing, {band.tail, 0u});
                    s_drawingBands = false;
                    args.restore(ctx);
                }
            }
            s_meshDraw(rdram, ctx, runtime);
        }

        void trackGone(uint8_t *, R5900Context *, PS2Runtime *, uint32_t)
        {
            s_bandGems.clear(), s_bands.clear();
            s_bandMat = s_dividersMat = 0u;
        }
    }

    bool isBandTail(uint8_t *rdram, uint32_t self)
    {
        const auto found = s_bands.find(self);
        return found != s_bands.end() && load<uint32_t>(rdram, found->second.rep + gemRep::kTail) == found->second.tail;
    }

    void installLaneBands(PS2Runtime &runtime, const Addresses &addresses)
    {
        s_addresses = &addresses;
        addToTrackScene(addBandMat);
        onGemAdded(gemAdded);
        replaceKeeping(runtime, addresses.gemRepEnableDuration, &enableDuration, s_enableDuration);
        replaceKeeping(runtime, addresses.gemHit, &gemHit, s_gemHit);
        EntryHook<TailSyncTag>::install(runtime, addresses.psMeshSync, onTailSync);
        onTrackGone(trackGone);
        replaceKeeping(runtime, addresses.psMeshDrawShowing, &meshDraw, s_meshDraw);
        replaceKeeping(runtime, addresses.trackDraw, &trackDraw, s_trackDraw);
    }
}
