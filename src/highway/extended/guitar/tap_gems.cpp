// A custom song's tap (gameplay/extended/guitar/gem_kinds.h) is drawn as dark
// glass under a solid HOPO top of its colour, whether shown as a gem, a star
// phrase's star, a gem under star power (cyan) or star power's cyan star
// (highway/cyan_stars.h); its sustain GH2's own with a bright outline round it.
// Each part it may show has a tap_ copy in the track scene
// (highway/extended/guitar/tap_pieces.h), drawn kTapAlpha see-through. A tap
// shows its copies over whatever GH2 gives it, each frame; a missed one shows
// GH2's miss, which has none. An open tap is
// highway/extended/guitar/open_gems', on the glass bodies made there, its
// sustain outlined as a lane's.
//
// Which gems are taps is found as GemManager::AdvanceEnd adds each to the
// track: its GameGem's tick and frets. A rep lent a copy is given back what
// it had as its gem lets its reps go (Gem::RemoveRep), as the parts no
// lane's differ in are not set again when the pool lends it next.

#include "highway/extended/guitar/tap_gems.h"

#include "gameplay/extended/guitar/gem_kinds.h"
#include "guest.h"
#include "guest/beatmatch.h"
#include "highway/extended/guitar/open_gems.h"
#include "highway/extended/guitar/tap_pieces.h"
#include "highway/gem_looks.h"
#include "hook.h"
#include "milo/layout.h"
#include "render/native_mesh.h"

#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace gh2::guitar
{
    namespace
    {
        constexpr float kTapAlpha = 0.5f, kStarAlpha = 0.75f;
        // Every part but the gem's shine (glow.mesh): its cap, its gem, and
        // a star's base, top, body and shine.
        constexpr uint32_t kBodyParts[] = {gemRep::kCap,     gemRep::kGem,      gemRep::kStarBase,
                                           gemRep::kStarTop, gemRep::kStarBody, gemRep::kStarShine};

        // The Gems on the track that are taps.
        std::set<uint32_t> s_taps;

        void gemAdded(uint8_t *rdram, R5900Context *, PS2Runtime *, const GemAdded &added)
        {
            if (!playingCustom())
                return void(s_taps.erase(added.gem));
            const guest::GameGem game(rdram, added.game);
            if (added.game != 0u && isTap(added.track, static_cast<uint32_t>(game.tick()), game.slots()))
            {
                s_taps.insert(added.gem);
                // Drawn as a HOPO even first or after a gap, as it is played
                // (gameplay/extended/guitar/hammer_ons): GemManager::SetupGems
                // makes a Gem one only with a playable gem before it.
                store<uint32_t>(rdram, added.gem + gem::kFrets, load<uint32_t>(rdram, added.gem + gem::kFrets) | gem::kHopo);
            }
            else
                s_taps.erase(added.gem);
        }

        // A tap_ material made glass, once each, as its scene is first
        // drawn from.
        std::set<uint32_t> s_glassed;
        void glass(uint8_t *rdram, uint32_t mat)
        {
            if (!s_glassed.insert(mat).second)
                return;
            // A star's shine (star2.mat) is drawn over everything (kZForce),
            // white over GH2's white top; over a tap's coloured one it shows,
            // so a tap's is hidden by what is in front of it.
            if (std::string(objectName(rdram, mat)) == tapped("star2.mat"))
                store<uint32_t>(rdram, mat + ::milo::mat::kZMode, ::milo::mat::kZTransparent);
            // A cap and a star's top solid, unblended: top.mat blends by
            // alpha, and over a coloured star its top would show the star.
            const std::string name(objectName(rdram, mat));
            if (tapPieces().solid.count(name))
                return store<uint32_t>(rdram, mat + ::milo::mat::kBlend, ::milo::mat::kBlendSrc);
            store<uint32_t>(rdram, mat + ::milo::mat::kBlend, ::milo::mat::kBlendSrcAlpha);
            store<float>(rdram, mat + ::milo::mat::kColor + 12u, tapPieces().starGlass.count(name) ? kStarAlpha : kTapAlpha);
        }

        // What a tap's rep had before its copies, to give back: each part's
        // template, by rep and part.
        std::map<std::pair<uint32_t, uint32_t>, uint32_t> s_partWas;

        // A tap rep's body part shown as its copy, where it shows a mesh that
        // has one: a gem's in its own template, a star's as a multimesh
        // turning with the star it copies.
        void dressPart(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t rep, uint32_t part)
        {
            const uint32_t object = partObject(rdram, rep, part);
            const std::string name(objectName(rdram, object));
            if (object == 0u || name.rfind(kTapPrefix, 0u) == 0u)
                return;
            // A HOPO cap banded; under star power, where every lane's is
            // the blue one, banded in star power's cyan.
            std::string as = tapped(name);
            if (part == gemRep::kCap)
            {
                const bool bonus = load<uint32_t>(rdram, rep + gemRep::kType) == gemRep::kTypeBonus;
                const std::string lane = name.substr(0u, name.find('_'));
                if (name.size() < 10u || name.compare(name.size() - 10u, 10u, "_hopo.mesh") != 0)
                    return;
                as = kTapPrefix + (bonus ? std::string("bonus") : lane) + "_cap.mesh";
            }
            const uint32_t scene = load<uint32_t>(rdram, object + ::milo::object::kDir);
            const auto mat = tapPieces().meshMat.find(as);
            const uint32_t found = findObject(rdram, ctx, runtime, scene, as);
            const uint32_t foundMat =
                mat != tapPieces().meshMat.end() ? findObject(rdram, ctx, runtime, scene, mat->second) : 0u;
            if (found == 0u || foundMat == 0u)
                return;
            glass(rdram, foundMat);
            // What it shows now, not a tap's: GH2's or the cyan star's latest.
            s_partWas[{rep, part}] = partTemplate(rdram, rep, part);
            // The gem's own two parts each with its mesh; those after, a
            // star's, as a multimesh turning with what it copies.
            if (part == gemRep::kCap || part == gemRep::kGem)
                showPartAs(rdram, ctx, runtime, rep, part, found);
            else
                showPartAs(rdram, ctx, runtime, rep, part, found, true, object);
        }

        // A tap's sustain is GH2's own, and over it an outline: a band along
        // each outer edge and round its end, the end a half circle across
        // it, drawn with the tail (a native mesh rider) so it follows the
        // tail as Poll lays it, whammied or not, in Poll's colour for it.
        //
        // Each tap rep with a tail, by the tail.
        std::map<uint32_t, uint32_t> s_tapTails;
        std::set<uint32_t> s_tapReps;
        using gemRep::kTailAcross;
        // The band's width and its lift over the tail's top (track units, a
        // lane 4 across, a tail about 0.86), its brightness over Poll's
        // colour, and the half circle's steps.
        constexpr float kOutlineWidth = 0.16f, kOutlineLift = 0.04f, kOutlineBright = 2.2f;
        constexpr int kOutlineArc = 12;
        // An open's (its sheet about 15 across), and its round end's depth
        // along the track over its half width.
        constexpr float kOpenOutlineWidth = 0.6f, kOpenArcAlong = 0.25f;
        // The tail under it, its alpha scaled by this.
        constexpr float kTailAlpha = 0.4f;

        void rideTapTail(uint8_t *rdram, uint32_t mesh, const MeshData &geometry, std::vector<std::shared_ptr<const MeshData>> &out)
        {
            const auto found = s_tapTails.find(mesh);
            const size_t rows = geometry.verts.size() / kTailAcross;
            if (found == s_tapTails.end() || rows < 3u || !s_tapReps.count(found->second) ||
                load<uint32_t>(rdram, found->second + gemRep::kTail) != mesh)
                return;
            const auto at = [&](size_t row, size_t across) -> const Vertex & { return geometry.verts[row * kTailAcross + across]; };
            // A lane's tail traced along its clear edge, an open's (pulled
            // apart, highway/extended/guitar/open_tails) along its lit one,
            // where its sheet shows; an open's band wider, as its sheet is.
            // Each point drawn as a vertex whose spot of the texture is whole:
            // a lane's lit edge, an open's middle (tail2.tex is clear at the
            // edge).
            const bool open = isOpenRep(found->second);
            const size_t left = open ? 1u : 0u, right = kTailAcross - 1u - left, from = open ? 2u : 1u;
            const float band = open ? kOpenOutlineWidth : kOutlineWidth;
            const auto width = [&](size_t row) { return std::fabs(at(row, right).pos[0] - at(row, left).pos[0]); };
            float widest = 0.0f;
            for (size_t r = 0; r < rows; ++r)
                widest = std::max(widest, width(r));
            if (widest <= band * 2.0f)
                return;
            // Its rows as wide as it is, from its foot; the end the side its
            // rows narrow to a point.
            const bool tipLast = width(rows - 1u) <= width(0u);
            std::vector<size_t> body;
            for (size_t i = 0; i < rows; ++i)
            {
                const size_t r = tipLast ? i : rows - 1u - i;
                if (width(r) >= widest * 0.9f)
                    body.push_back(r);
                else if (!body.empty())
                    break;
            }
            if (body.size() < 2u)
                return;
            // The band's outer and inner points, up the left edge, round the
            // end, and back down the right.
            struct Point
            {
                float x, y, z, ix, iy;
                const Vertex *from;
            };
            std::vector<Point> line;
            const auto top = [&](size_t r) { return at(r, 2u).pos[2] + kOutlineLift; };
            for (const size_t r : body)
                line.push_back({at(r, left).pos[0], at(r, left).pos[1], top(r), at(r, left).pos[0] + band, at(r, left).pos[1],
                                &at(r, from)});
            // Rounded on toward its tip: from the foot, as the rows before
            // the end may all sit at it.
            const size_t end = body.back(), foot = body.front();
            const float cx = (at(end, left).pos[0] + at(end, right).pos[0]) * 0.5f, cy = at(end, left).pos[1];
            const float half = width(end) * 0.5f, forward = at(end, left).pos[1] >= at(foot, left).pos[1] ? 1.0f : -1.0f;
            // An open's end flatter along the track, as wide as it is.
            const float along = open ? kOpenArcAlong : 1.0f;
            for (int k = 1; k < kOutlineArc; ++k)
            {
                const float t = 3.14159265f * float(k) / float(kOutlineArc);
                const float dx = -std::cos(t), dy = forward * std::sin(t) * along, inner = half - band;
                line.push_back({cx + dx * half, cy + dy * half, top(end), cx + dx * inner, cy + dy * inner, &at(end, from)});
            }
            for (auto r = body.rbegin(); r != body.rend(); ++r)
            {
                const Vertex &edge = at(*r, right);
                line.push_back({edge.pos[0], edge.pos[1], top(*r), edge.pos[0] - band, edge.pos[1], &at(*r, from)});
            }
            // Poll's colour for its lit edge, brighter; a star's or one
            // under star power's in star power's cyan, at the brightness
            // Poll gives a lane's (about half its colour).
            const uint32_t type = load<uint32_t>(rdram, found->second + gemRep::kType);
            float colour[3];
            for (int c = 0; c < 3; ++c)
                colour[c] = (type == gemRep::kTypeStar || type == gemRep::kTypeBonus ? kStarPowerCyan[c] * 0.5f
                                                                                    : at(body.front(), 1u).color[c]) *
                            kOutlineBright;
            auto outline = std::make_shared<MeshData>();
            outline->transient = true;
            for (const Point &p : line)
                for (const bool inner : {false, true})
                {
                    Vertex v = *p.from;
                    v.pos[0] = inner ? p.ix : p.x;
                    v.pos[1] = inner ? p.iy : p.y;
                    v.pos[2] = p.z;
                    std::copy(colour, colour + 3, v.color);
                    v.color[3] = 1.0f;
                    outline->verts.push_back(v);
                }
            for (uint16_t i = 0; i + 1u < line.size(); ++i)
            {
                const uint16_t a = uint16_t(i * 2u), b = uint16_t(a + 1u), c = uint16_t(a + 2u), d = uint16_t(a + 3u);
                for (const uint16_t k : {a, b, c, b, d, c})
                    outline->indices.push_back(k);
            }
            out.push_back(std::move(outline));
        }

        std::map<uint32_t, uint32_t> s_syncTails; // tap tails, by the this Sync takes

        // PsMesh::Sync(int): a tap's tail a little see-through under its
        // outline (which is drawn whole), each vertex's alpha as Poll wrote
        // it, scaled. Poll writes every vertex anew each frame, so once a
        // frame; a sync with no new write finds them scaled already.
        std::map<uint32_t, std::vector<float>> s_tailAlpha; // as last left, by tail
        struct TailSyncTag;
        void onTailSync(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            const auto found = s_syncTails.find(GPR_U32(ctx, 4));
            if (found == s_syncTails.end())
                return;
            const uint32_t mesh = found->second;
            const auto rep = s_tapTails.find(mesh);
            if (rep == s_tapTails.end() || !s_tapReps.count(rep->second) || load<uint32_t>(rdram, rep->second + gemRep::kTail) != mesh)
                return;
            const uint32_t verts = load<uint32_t>(rdram, mesh + ::milo::mesh::kVerts);
            const int32_t count = load<int32_t>(rdram, mesh + ::milo::mesh::kVertCount);
            if (verts == 0u || count <= 0)
                return;
            const auto alpha = [&](int32_t i)
            { return verts + uint32_t(i) * ::milo::mesh::kVertSize + ::milo::mesh::kVertColor + 12u; };
            std::vector<float> &left = s_tailAlpha[mesh];
            bool same = left.size() == size_t(count);
            for (int32_t i = 0; same && i < count; ++i)
                same = load<float>(rdram, alpha(i)) == left[size_t(i)];
            if (same)
                return;
            left.resize(size_t(count));
            for (int32_t i = 0; i < count; ++i)
            {
                const float a = load<float>(rdram, alpha(i)) * kTailAlpha;
                store<float>(rdram, alpha(i), a);
                left[size_t(i)] = a;
            }
        }

        // Gem::Poll, each frame for each gem on the track: a tap's reps and
        // sustain dressed as glass.
        struct GemPollTag;
        void onGemPoll(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t gem = GPR_U32(ctx, 4);
            if (!s_taps.count(gem))
                return;
            // Poll's own, put back for it once this has called the guest.
            float floats[5];
            for (int i = 0; i < 5; ++i)
                floats[i] = ctx->f[12 + i];
            const auto restore = [&]
            {
                SET_GPR_U32(ctx, 4, gem);
                for (int i = 0; i < 5; ++i)
                    ctx->f[12 + i] = floats[i];
            };
            // An open tap's sustain outlined too (its gem and tail are
            // highway/extended/guitar/open_gems' and open_tails', its glass gem
            // shown by open_gems), its tail left as those make it.
            if (const uint32_t first = load<uint32_t>(rdram, gem + gem::kReps);
                first != load<uint32_t>(rdram, gem + gem::kRepsEnd) && isOpenRep(load<uint32_t>(rdram, first)))
            {
                const uint32_t rep = load<uint32_t>(rdram, first);
                s_tapReps.insert(rep);
                // Its gem's materials see-through.
                if (const uint32_t object = partObject(rdram, rep, gemRep::kGem))
                    for (const std::string &mat : tapPieces().openGlass)
                        if (const uint32_t found =
                                findObject(rdram, ctx, runtime, load<uint32_t>(rdram, object + ::milo::object::kDir), mat))
                            glass(rdram, found);
                if (const uint32_t tail = load<uint32_t>(rdram, rep + gemRep::kTail))
                    s_tapTails[tail] = rep;
                return restore();
            }
            for (uint32_t r = load<uint32_t>(rdram, gem + gem::kReps), end = load<uint32_t>(rdram, gem + gem::kRepsEnd); r < end;
                 r += 4u)
            {
                const uint32_t rep = load<uint32_t>(rdram, r);
                s_tapReps.insert(rep);
                for (const uint32_t part : kBodyParts)
                    dressPart(rdram, ctx, runtime, rep, part);
                if (const uint32_t tail = load<uint32_t>(rdram, rep + gemRep::kTail))
                {
                    s_tapTails[tail] = rep;
                    s_syncTails[syncThis(rdram, tail)] = tail;
                }
            }
            restore();
        }

        // Gem::RemoveRep(GemRepPool &): its reps given back what they had.
        struct RemoveRepTag;
        void onRemoveRep(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const EntryArgs args(ctx);
            const uint32_t gem = args.a[0];
            for (uint32_t r = load<uint32_t>(rdram, gem + gem::kReps), end = load<uint32_t>(rdram, gem + gem::kRepsEnd); r < end;
                 r += 4u)
            {
                const uint32_t rep = load<uint32_t>(rdram, r);
                for (const uint32_t part : kBodyParts)
                    if (const auto was = s_partWas.find({rep, part}); was != s_partWas.end())
                    {
                        if (objectName(rdram, partObject(rdram, rep, part)).rfind(kTapPrefix, 0u) == 0u)
                            setPartTemplate(rdram, ctx, runtime, rep, part, was->second);
                        s_partWas.erase(was);
                    }
                s_tapReps.erase(rep);
                if (const uint32_t tail = load<uint32_t>(rdram, rep + gemRep::kTail))
                {
                    s_tapTails.erase(tail);
                    s_tailAlpha.erase(tail);
                }
            }
            args.restore(ctx);
        }

        // As the track and its scene go, its reps with it.
        void trackGone(uint8_t *, R5900Context *, PS2Runtime *, uint32_t)
        {
            s_taps.clear(), s_glassed.clear(), s_partWas.clear(), s_tapTails.clear(), s_tapReps.clear(), s_syncTails.clear(), s_tailAlpha.clear();
        }
    }

    bool isTapGem(uint32_t gem)
    {
        return s_taps.count(gem) != 0u;
    }

    void installTapGems(PS2Runtime &runtime, const Addresses &addresses)
    {
        addTapPieces();
        onGemAdded(gemAdded);
        EntryHook<GemPollTag>::install(runtime, addresses.gemPoll, onGemPoll);
        EntryHook<RemoveRepTag>::install(runtime, addresses.gemRemoveRep, onRemoveRep);
        onTrackGone(trackGone);
        addMeshRider(rideTapTail);
        EntryHook<TailSyncTag>::install(runtime, addresses.psMeshSync, onTailSync);
    }
}
