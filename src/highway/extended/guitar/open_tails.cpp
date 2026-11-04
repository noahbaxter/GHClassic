// GemRep's tail, a mesh from GemRepPool's (CreateTail, 0x159b40), lent each
// gem with a sustain and given its lane's look by SetType: rows along it of
// six vertices across, a rounded tube 0.86 wide, its edges clear and its top
// two the flat middle. GemRep::Poll (0x158e18) writes each vertex every frame
// from its GemRepTemplate's row (+0x244), across times the whammy's wobble,
// then syncs it (PsMesh::Sync, 0x1592ec). An open's is pulled apart there,
// each half most of the way to an outer lane, its middle stretched between
// them, its colour its vertices' or a miss's once let go.
//
// Its glow, a RndLine shown while the gem is held (GemRep::Hit, 0x158cc0),
// has its mesh (+0x114) made each draw in screen space (UpdateLine,
// 0x1bb8f0), two vertices across at each point and, with caps (+0x118), two
// more at each end, pushed out along the line as far as it is wide. An
// open's is widened as its tail is, its caps drawn back to a lane's.
//
// The hooks on what Poll calls are on entry; those after GemRep's own run it
// to its end first (callOriginal).

#include "highway/extended/guitar/open_tails.h"

#include "guest.h"
#include "highway/extended/guitar/lane_bands.h"
#include "highway/extended/guitar/open_gems.h"
#include "highway/gem_looks.h"
#include "hook.h"
#include "milo/layout.h"
#include "render/native_mesh.h"

#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <memory>
#include <vector>

namespace gh2::guitar
{
    namespace
    {
        const Addresses *s_addresses = nullptr;

        // Colours are RGB, 1 a lane's full (more glows brighter); lengths are
        // track units, a lane 4 across.

        // The sustain's width: each half pulled out this far from the middle
        // lane's (about 15 across).
        constexpr float kTailSpan = 7.0f;
        // Its middle's opacity, its colours (a phrase's edges cyan, all cyan
        // under star power), and how far its end curves forward.
        constexpr float kTailMiddleAlpha = 0.55f;
        constexpr float kTailPurple[3] = {0.6f, 0.2f, 1.0f};
        constexpr const float *kTailCyan = kStarPowerCyan;
        constexpr float kTailCap = 7.0f;
        // The edges' cyan round that end, this many times as thick as at the
        // sides (the far track foreshortened, about 3 times reads even).
        constexpr float kTailEndRing = 6.0f;
        // A phrase's held sustain, its edges' glow: their colour, the
        // outermost's opacity, how far it and the edge widen.
        constexpr float kTailCyanLit[3] = {0.5f, 2.5f, 2.5f}, kTailGlowAlpha = 0.7f;
        constexpr float kTailGlowOut = 0.7f, kTailGlowWide = 0.3f;
        // The glow along a held sustain: its colour (star power's its own
        // cyan), how much wider than the sustain, and how far it runs past
        // the sustain's end (a share of a lane's glow's).
        constexpr float kGlowPurple[3] = {0.6f, 0.15f, 1.0f};
        constexpr float kGlowWider = 3.5f, kGlowCap = 1.0f;
        // Whammied, the glow brightens with GH2's whammy pulse (smooth, 0 at
        // rest to 1 at its fullest), this much at its fullest.
        constexpr float kGlowPulseBright = 0.6f;
        // A held sustain whammied: down its middle a lane's sustain, in
        // purple (a phrase's or under star power cyan), swelling as GH2
        // swells a lane's (its wobble), from nothing at rest out to the
        // sustain's edges at kSwellFull's wobble. Each row's width the
        // average of this many rows each side, eased this far toward it
        // each frame, so the bulges taper.
        constexpr float kSwellPurple[3] = {0.8f, 0.35f, 1.6f};
        constexpr float kSwellFull = 4.0f;
        constexpr int kSwellBlur = 3;
        constexpr float kSwellEase = 0.25f;
        // At its widest it reaches this share of the way to the edges, and
        // stands this many times as tall as a lane's sustain.
        constexpr float kSwellReach = 0.85f, kSwellRise = 3.0f;
        // Each row's reach bowed out (1 - (1 - reach) to this power), so its
        // front comes round rather than to a point.
        constexpr float kSwellBlunt = 3.0f;
        // Three faces across, as a lane's: a side, the top and a side, the
        // top this share of its width. Each face this bright, as GH2 lights
        // a lane's swollen sustain, its left two thirds its right.
        constexpr float kSwellTop = 1.0f / 3.0f;
        constexpr float kSwellShade[3] = {0.63f, 0.82f, 1.0f};
        // Above the sheet, which it would otherwise lose to in depth.
        constexpr float kSwellLift = 0.05f;

        // A row's vertices, each from its edge in on the template, and moved
        // this much less, pulled apart: the lit edge a quarter in from the
        // clear one, the middle a lane's fifth further.
        using gemRep::kTailAcross;
        constexpr float kTailTemplate[3] = {0.429816f, 0.369943f, 0.156495f};
        constexpr float kTailIn[3] = {0.0f, -0.62f, -1.16f};
        // A pulled apart row's clear and lit edges, out from its middle
        // vertex (kTailTemplate plus kTailIn).
        constexpr float kTailEdgeFromMiddle[2] = {1.4333f, 0.7534f};
        // A vertex this far out is apart already, where Poll left a row as
        // it was.
        constexpr float kTailNarrow = 2.0f;
        // An open's white (tail_white), coloured by its vertices. Once let
        // go, as its material (tail_miss).
        constexpr const char *kOpenTailMat = "tail_white.mat", *kMissTail = "tail_miss.mat";
        constexpr float kTailPlain[3] = {1.0f, 1.0f, 1.0f};
        // A clear edge's alpha, as Poll writes it.
        float s_clearAlpha = -1.0f;

        // A lane's tail ends in a point, its last row all at the middle, a
        // wide one's flat across: its last rows laid as a half ellipse
        // instead, kTailCap long (never past the row before them), each row
        // the row before's, narrowed about its middle, so its edges and
        // their cyan follow the curve. Laid whole each time from that row,
        // which no curve touches, as Poll leaves a row it is done with.
        constexpr uint32_t kTailCapRows = 6u;

        void roundEnd(uint8_t *rdram, uint32_t verts, uint32_t rows)
        {
            if (rows < kTailCapRows + 2u)
                return;
            const auto at = [&](uint32_t row, uint32_t across)
            { return verts + (row * kTailAcross + across) * ::milo::mesh::kVertSize; };
            const uint32_t from = rows - kTailCapRows - 1u;
            const float end = load<float>(rdram, at(rows - 1u, 0u) + ::milo::mesh::kVertPos + 4u);
            const float cap = std::min(kTailCap, end - load<float>(rdram, at(from, 0u) + ::milo::mesh::kVertPos + 4u));
            const float left = load<float>(rdram, at(from, 0u)), right = load<float>(rdram, at(from, kTailAcross - 1u));
            const float middle = (left + right) * 0.5f, half = std::fabs(right - left) * 0.5f;
            const float lit = half - std::fabs(load<float>(rdram, at(from, 1u)) - middle);
            if (cap <= 0.0f)
                return;
            for (uint32_t r = 0; r < kTailCapRows; ++r)
            {
                const float angle = 1.5707964f * float(r) / float(kTailCapRows - 1u);
                const float across = std::cos(angle), up = std::sin(angle);
                for (uint32_t j = 0; j < kTailAcross; ++j)
                {
                    const uint32_t v = at(from + 1u + r, j), base = at(from, j);
                    const float x = load<float>(rdram, base + ::milo::mesh::kVertPos);
                    // Each vertex on its own curve, as far in from the outer
                    // one at the end as at the sides, so the edges (and their
                    // cyan) run round the end past the middle. The cyan, from
                    // the lit edge in, kTailEndRing times as thick round the
                    // end.
                    const float raw = half - std::fabs(x - middle);
                    const float in = std::min(raw, lit) + std::max(0.0f, raw - lit) * kTailEndRing;
                    store<float>(rdram, v + ::milo::mesh::kVertPos, middle + (x - middle) * across);
                    store<float>(rdram, v + ::milo::mesh::kVertPos + 4u, end - cap + std::max(0.0f, cap - in) * up);
                    std::memcpy(getMemPtr(rdram, v + ::milo::mesh::kVertColor),
                                getMemPtr(rdram, base + ::milo::mesh::kVertColor), 16u);
                }
            }
        }

        // A held tail's swell, by its mesh: whether it has one, cyan or
        // purple, each row's wobble as Poll last wrote it (its place over
        // the template's, 1 at rest), and each row's reach, 0 at rest to 1
        // at the sustain's edges, blurred and eased.
        struct Swell
        {
            uint32_t rep = 0u;
            bool on = false, cyan = false;
            std::vector<float> wobble, reach;
        };
        std::map<uint32_t, Swell> s_swells;

        void pullApart(uint8_t *rdram, uint32_t rep, uint32_t mesh)
        {
            const uint32_t type = load<uint32_t>(rdram, rep + gemRep::kType);
            const bool held = load<uint32_t>(rdram, rep + gemRep::kState) == gemRep::kHeld;
            const bool missed = objectName(rdram, load<uint32_t>(rdram, mesh + ::milo::mesh::kMat)) == kMissTail;
            const uint32_t verts = load<uint32_t>(rdram, mesh + ::milo::mesh::kVerts);
            const int32_t count = load<int32_t>(rdram, mesh + ::milo::mesh::kVertCount);
            if (verts == 0u || count < int32_t(kTailAcross))
                return;
            Swell &swell = s_swells[mesh];
            // A tail lent again starts at rest.
            if (swell.rep != rep)
                swell = Swell{};
            swell.rep = rep;
            swell.on = held && !missed;
            swell.cyan = type == gemRep::kTypeStar || type == gemRep::kTypeBonus;
            swell.wobble.resize(uint32_t(count) / kTailAcross, 1.0f);
            for (int32_t i = 0; i < count; ++i)
            {
                const uint32_t v = verts + uint32_t(i) * ::milo::mesh::kVertSize;
                const float x = load<float>(rdram, v + ::milo::mesh::kVertPos);
                if (std::fabs(x) >= kTailNarrow)
                    continue;
                // Its place in its row: 0 the clear edge, 1 the lit one, 2
                // the middle. Its side by its place, as a tail's tip row is
                // all at the middle.
                const uint32_t k = std::min(uint32_t(i) % kTailAcross, kTailAcross - 1u - uint32_t(i) % kTailAcross);
                // Its wobble, before the round end (roundEnd draws it in).
                if (uint32_t(i) % kTailAcross == 0u && uint32_t(i) / kTailAcross + kTailCapRows + 1u < uint32_t(count) / kTailAcross)
                    swell.wobble[uint32_t(i) / kTailAcross] = std::fabs(x) / kTailTemplate[0];
                // Placed from the template, not Poll's whammy wobble (its
                // place times the wobble, 0x15905c), which across a sheet
                // this wide steps from row to row: an open's whammy is its
                // swell's (rideTail).
                const float out = kTailTemplate[k] + kTailSpan + kTailIn[k];
                store<float>(rdram, v + ::milo::mesh::kVertPos, uint32_t(i) % kTailAcross < kTailAcross / 2u ? -out : out);
                const uint32_t alpha = v + ::milo::mesh::kVertColor + 12u;
                if (k == 0u && s_clearAlpha < 0.0f)
                    s_clearAlpha = load<float>(rdram, alpha);
                // Its middle see-through, a miss's grey as a lane's.
                if (k == 2u && !missed)
                    store<float>(rdram, alpha, load<float>(rdram, alpha) * kTailMiddleAlpha);
            }
            // The swell's reach, by the rows before the round end.
            const int32_t body = count / int32_t(kTailAcross) - int32_t(kTailCapRows) - 1;
            swell.reach.resize(swell.wobble.size(), 0.0f);
            for (int32_t r = 0; r < body; ++r)
            {
                float sum = 0.0f;
                int32_t n = 0;
                for (int32_t b = std::max(0, r - kSwellBlur); b <= std::min(body - 1, r + kSwellBlur); ++b, ++n)
                    sum += std::clamp((swell.wobble[size_t(b)] - 1.0f) / (kSwellFull - 1.0f), 0.0f, 1.0f);
                const float target = swell.on && n > 0 ? sum / float(n) : 0.0f;
                swell.reach[size_t(r)] += (target - swell.reach[size_t(r)]) * kSwellEase;
            }
            // Its colours every time, as Poll leaves a held row as it was. A
            // phrase's edges, held, lit and their clear edge shown a little
            // further out, a glow down each side; its middle as it was.
            const uint32_t rows = uint32_t(count) / kTailAcross;
            for (uint32_t i = 0; i < rows * kTailAcross; ++i)
            {
                const uint32_t v = verts + i * ::milo::mesh::kVertSize, j = i % kTailAcross;
                const uint32_t k = std::min(j, kTailAcross - 1u - j);
                const bool glowing = held && type == gemRep::kTypeStar && k != 2u && !missed;
                const float *rgb = glowing  ? kTailCyanLit
                                   : missed ? kTailPlain
                                   : type == gemRep::kTypeBonus || (k != 2u && type == gemRep::kTypeStar) ? kTailCyan
                                                                                                          : kTailPurple;
                const uint32_t color = v + ::milo::mesh::kVertColor;
                for (uint32_t c = 0; c < 3u; ++c)
                    store<float>(rdram, color + c * 4u, rgb[c]);
                if (k == 2u || i / kTailAcross == rows - 1u)
                    continue;
                if (k == 0u && s_clearAlpha >= 0.0f)
                    store<float>(rdram, color + 12u, glowing ? kTailGlowAlpha : s_clearAlpha);
                // Placed from the middle vertex on its side, as pulled apart;
                // held, both further out, the band from the lit edge to the
                // middle (the glow, cyan into purple) wider.
                const float side = j < kTailAcross / 2u ? -1.0f : 1.0f;
                const uint32_t middle = verts + (j < kTailAcross / 2u ? i + 2u - k : i - 2u + k) * ::milo::mesh::kVertSize;
                const float from = kTailEdgeFromMiddle[k] + (glowing ? kTailGlowWide + (k == 0u ? kTailGlowOut : 0.0f) : 0.0f);
                store<float>(rdram, v + ::milo::mesh::kVertPos, load<float>(rdram, middle + ::milo::mesh::kVertPos) + side * from);
            }
            roundEnd(rdram, verts, rows);
        }

        // A held tail's swell, drawn with it: down its middle as a lane's,
        // each row three faces across (kSwellTop), out to the sheet's edges
        // by its reach; round the end, along the sheet's outer edge,
        // narrowing to its tip as it does. None while no row reaches out.
        void rideTail(uint8_t *rdram, uint32_t mesh, const MeshData &geometry, std::vector<std::shared_ptr<const MeshData>> &out)
        {
            const auto found = s_swells.find(mesh);
            const size_t rows = geometry.verts.size() / kTailAcross;
            if (found == s_swells.end() || rows < kTailCapRows + 2u || found->second.reach.size() != rows ||
                !isOpenRep(found->second.rep) || load<uint32_t>(rdram, found->second.rep + gemRep::kTail) != mesh ||
                *std::max_element(found->second.reach.begin(), found->second.reach.end()) < 0.01f)
                return;
            const Swell &swell = found->second;
            const float *rgb = swell.cyan ? kTailCyanLit : kSwellPurple;
            const auto across = [&](size_t row)
            {
                const Vertex &left = geometry.verts[row * kTailAcross], &right = geometry.verts[row * kTailAcross + kTailAcross - 1u];
                return std::fabs(right.pos[0] - left.pos[0]);
            };
            const float full = across(0u);
            auto middle = std::make_shared<MeshData>();
            middle->transient = true;
            for (size_t r = 0; r < rows; ++r)
            {
                const Vertex &left = geometry.verts[r * kTailAcross], &right = geometry.verts[r * kTailAcross + kTailAcross - 1u];
                const float narrow = full > 0.0f ? across(r) / full : 1.0f;
                const float reach = 1.0f - std::pow(1.0f - swell.reach[std::min(r, rows - kTailCapRows - 2u)], kSwellBlunt);
                const float half = full * 0.5f * kSwellReach * reach * narrow;
                const float rise = 1.0f + (kSwellRise - 1.0f) * reach;
                // Its foot at the sheet, its top as high over it as the
                // middle's, raised.
                const float centre = (left.pos[0] + right.pos[0]) * 0.5f, foot = left.pos[2] + kSwellLift;
                const float top = left.pos[2] + (geometry.verts[r * kTailAcross + 2u].pos[2] - left.pos[2]) * rise + kSwellLift;
                const float xs[4] = {centre - half, centre - half * kSwellTop, centre + half * kSwellTop, centre + half};
                const float zs[4] = {foot, top, top, foot};
                // Each face its own two vertices, so its shade stops at its
                // edges; all on the texture's middle, plain (from its edge
                // in, it is see-through, then a bright band).
                for (size_t f = 0; f < 3u; ++f)
                    for (size_t e = 0; e < 2u; ++e)
                    {
                        Vertex v = geometry.verts[r * kTailAcross + 2u];
                        v.pos[0] = xs[f + e];
                        v.pos[2] = zs[f + e];
                        for (uint32_t c = 0; c < 3u; ++c)
                            v.color[c] = rgb[c] * kSwellShade[f];
                        v.color[3] = 1.0f;
                        middle->verts.push_back(v);
                    }
            }
            for (size_t r = 0; r + 1u < rows; ++r)
                for (size_t f = 0; f < 3u; ++f)
                {
                    const uint16_t a = uint16_t(r * 6u + f * 2u), b = uint16_t(a + 1u), c = uint16_t(a + 6u), d = uint16_t(a + 7u);
                    for (uint16_t i : {a, b, c, b, d, c})
                        middle->indices.push_back(i);
                }
            out.push_back(std::move(middle));
        }

        // Each open rep's tail and glow, by the this Sync takes and by line:
        // still its while the rep is an open's and has them.
        struct Lent
        {
            uint32_t rep, object;
        };
        std::map<uint32_t, Lent> s_tailReps, s_glowReps;

        const Lent *stillOpen(uint8_t *rdram, const std::map<uint32_t, Lent> &reps, uint32_t key, uint32_t at)
        {
            const auto found = reps.find(key);
            if (found == reps.end() || !isOpenRep(found->second.rep) ||
                load<uint32_t>(rdram, found->second.rep + at) != found->second.object)
                return nullptr;
            return &found->second;
        }

        // RndLine: its width, its points (a vector, 0x50 apart, each place
        // first, 0x1bce50), its mesh and caps, and its Mat. GemRepTemplate:
        // a lane's glow's width.
        constexpr uint32_t kLineWidth = 0x100u, kLinePoints = 0x104u, kLinePointSize = 0x50u, kLineMesh = 0x114u,
                           kLineCaps = 0x118u, kTemplateGlowWidth = 0x22cu;

        // Each open glow's mesh, by the this its Sync takes: its line, and
        // its caps' length as a share of a lane's. A mesh is synced more than
        // once a draw: its first cap's place as last drawn back, so it is
        // drawn back once.
        struct GlowMesh
        {
            uint32_t line;
            float caps;
            float drawn[3];
        };
        std::map<uint32_t, GlowMesh> s_glowMeshes;

        void drawCapsBack(uint8_t *rdram, uint32_t mesh, GlowMesh &glow)
        {
            const uint32_t verts = load<uint32_t>(rdram, mesh + ::milo::mesh::kVerts);
            const int32_t count = load<int32_t>(rdram, mesh + ::milo::mesh::kVertCount);
            if (verts == 0u || count < 8 ||
                std::memcmp(getMemPtr(rdram, verts + ::milo::mesh::kVertPos), glow.drawn, sizeof glow.drawn) == 0)
                return;
            // Its first two and last two, each from the one along from it.
            const int32_t ends[4][2] = {{0, 2}, {1, 3}, {count - 2, count - 4}, {count - 1, count - 3}};
            for (const auto &[cap, from] : ends)
            {
                const uint32_t at = verts + uint32_t(cap) * ::milo::mesh::kVertSize + ::milo::mesh::kVertPos;
                const uint32_t base = verts + uint32_t(from) * ::milo::mesh::kVertSize + ::milo::mesh::kVertPos;
                for (uint32_t c = 0; c < 3u; ++c)
                {
                    const float b = load<float>(rdram, base + c * 4u);
                    store<float>(rdram, at + c * 4u, b + (load<float>(rdram, at + c * 4u) - b) * glow.caps);
                }
            }
            std::memcpy(glow.drawn, getMemPtr(rdram, verts + ::milo::mesh::kVertPos), sizeof glow.drawn);
        }

        // PsMesh::Sync(int): an open's tail pulled apart before it is taken,
        // and its glow's caps drawn back.
        struct TailSyncTag;
        void onTailSync(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            const uint32_t self = GPR_U32(ctx, 4);
            if (isBandTail(rdram, self))
                return;
            if (const Lent *tail = stillOpen(rdram, s_tailReps, self, gemRep::kTail))
                return pullApart(rdram, tail->rep, tail->object);
            const auto glow = s_glowMeshes.find(self);
            if (glow != s_glowMeshes.end() && stillOpen(rdram, s_glowReps, glow->second.line, gemRep::kGlow) &&
                load<uint32_t>(rdram, glow->second.line + kLineCaps) != 0u)
                drawCapsBack(rdram, load<uint32_t>(rdram, glow->second.line + kLineMesh), glow->second);
        }

        // RndLine::SetPointColor(int, const Color &), with which Poll gives
        // the glow its lane's colour (0x159480): an open's purple, brighter
        // as it is whammied, or star power's cyan as every lane's. A phrase's
        // shows before it is hit, round a lane's tail but through an open's
        // middle: an open's only while held.
        struct GlowColorTag;
        void onGlowColor(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            const EntryArgs args(ctx);
            const Lent *glow = stillOpen(rdram, s_glowReps, args.a[0], gemRep::kGlow);
            if (glow == nullptr)
                return;
            if (load<uint32_t>(rdram, glow->rep + gemRep::kState) != gemRep::kHeld)
                return store<float>(rdram, args.a[2] + 12u, 0.0f);
            if (load<uint32_t>(rdram, glow->rep + gemRep::kType) == gemRep::kTypeBonus)
                return;
            const float pulse = std::clamp((load<float>(rdram, glow->rep + gemRep::kPulse) - 1.0f) / 1.5f, 0.0f, 1.0f);
            for (uint32_t c = 0; c < 3u; ++c)
                store<float>(rdram, args.a[2] + c * 4u, kGlowPurple[c] * (1.0f + pulse * kGlowPulseBright));
        }

        // Each open glow's width as last widened, so a second draw before
        // Poll sets it again leaves it, and how much it is widened by.
        struct Widened
        {
            float width = 0.0f, by = 1.0f;
        };
        std::map<uint32_t, Widened> s_widened;

        // RndLine::DrawShowing(DrawType): an open's glow widened by its
        // tail's spread, still pulsing.
        struct GlowDrawTag;
        void onGlowDraw(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            const uint32_t line = GPR_U32(ctx, 4);
            const Lent *glow = stillOpen(rdram, s_glowReps, line, gemRep::kGlow);
            if (glow == nullptr)
                return;
            // Poll's width (its lane's, pulsing as it is whammied) and the
            // open's spread on it.
            const float width = load<float>(rdram, line + kLineWidth);
            Widened &widened = s_widened[line];
            if (width != widened.width)
            {
                const float base = load<float>(rdram, load<uint32_t>(rdram, glow->rep + gemRep::kTemplate) + kTemplateGlowWidth);
                widened.width = base + kTailSpan + kGlowWider;
                widened.by = width > 0.0f ? widened.width / width : 1.0f;
                store<float>(rdram, line + kLineWidth, widened.width);
            }
            if (const uint32_t mesh = load<uint32_t>(rdram, line + kLineMesh))
                s_glowMeshes[syncThis(rdram, mesh)] = {line, kGlowCap / widened.by, {}};
        }

        // GemRep::SetDuration(float, float, float) ends the glow short of the
        // track's far end by four times its width (0x158bcc), for its cap: an
        // open's as a lane's, by its width unwidened.
        PS2Runtime::RecompiledFunction s_setDuration = nullptr;
        void setDuration(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t rep = GPR_U32(ctx, 4);
            const uint32_t line = isOpenRep(rep) ? load<uint32_t>(rdram, rep + gemRep::kGlow) : 0u;
            const auto widened = s_widened.find(line);
            const bool narrowed = line != 0u && widened != s_widened.end() &&
                                  load<float>(rdram, line + kLineWidth) == widened->second.width;
            if (narrowed)
                store<float>(rdram, line + kLineWidth, widened->second.width / widened->second.by);
            callOriginal(rdram, ctx, runtime, s_addresses->gemRepSetDuration, s_setDuration);
            if (narrowed)
                store<float>(rdram, line + kLineWidth, widened->second.width);
            // Its end drawn back to where the tail's round end starts, its
            // own round cap over it (drawCapsBack).
            if (line != 0u)
                if (const uint32_t points = load<uint32_t>(rdram, line + kLinePoints))
                {
                    const float start = load<float>(rdram, points + 4u);
                    const uint32_t end = points + kLinePointSize + 4u;
                    store<float>(rdram, end, std::max(start, load<float>(rdram, end) - kTailCap));
                }
        }

        // GemRep::EnableDuration, which lends the rep a tail.
        PS2Runtime::RecompiledFunction s_enableDuration = nullptr;
        void enableDuration(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t rep = GPR_U32(ctx, 4);
            callOriginal(rdram, ctx, runtime, s_addresses->gemRepEnableDuration, s_enableDuration);
            if (isOpenRep(rep))
                dressOpenTail(rdram, ctx, runtime, rep);
        }

        // As the track goes with its tails and glows.
        void trackGone(uint8_t *, R5900Context *, PS2Runtime *, uint32_t)
        {
            s_swells.clear(), s_tailReps.clear(), s_glowReps.clear(), s_glowMeshes.clear(), s_widened.clear();
        }
    }

    void dressOpenTail(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t rep)
    {
        const uint32_t tail = load<uint32_t>(rdram, rep + gemRep::kTail);
        if (tail == 0u)
            return;
        s_tailReps[syncThis(rdram, tail)] = {rep, tail};
        if (const uint32_t line = load<uint32_t>(rdram, rep + gemRep::kGlow))
            s_glowReps[line] = {rep, line};
        // Under star power, its cyan (tail_bonus), as SetType gave it.
        if (load<uint32_t>(rdram, rep + gemRep::kType) == gemRep::kTypeBonus)
            return;
        // Its purple, found in the scene its gem's meshes are from.
        const uint32_t cap = partObject(rdram, rep, gemRep::kCap);
        const uint32_t scene = cap ? load<uint32_t>(rdram, cap + ::milo::object::kDir) : 0u;
        if (const uint32_t mat = scene ? findObject(rdram, ctx, runtime, scene, kOpenTailMat) : 0u)
            runtime->callGuestFunction(rdram, ctx, s_addresses->rndMeshSetMat, {tail, mat});
    }

    void installOpenTails(PS2Runtime &runtime, const Addresses &addresses)
    {
        s_addresses = &addresses;
        addMeshRider(rideTail);
        replaceKeeping(runtime, addresses.gemRepEnableDuration, &enableDuration, s_enableDuration);
        replaceKeeping(runtime, addresses.gemRepSetDuration, &setDuration, s_setDuration);
        // After the mesh capture's, so a tail is pulled apart before it is
        // taken.
        EntryHook<TailSyncTag>::install(runtime, addresses.psMeshSync, onTailSync);
        EntryHook<GlowColorTag>::install(runtime, addresses.rndLineSetPointColor, onGlowColor);
        EntryHook<GlowDrawTag>::install(runtime, addresses.rndLineDrawShowing, onGlowDraw);
        onTrackGone(trackGone);
    }
}
