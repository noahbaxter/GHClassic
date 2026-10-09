// GH1's crowd in GH2's venues.
//
// GH1 keeps a card's place for each member of the crowd it draws flat:
// Crowd<nn>.mm, a MultiMesh 0 (a Draw 1, its mesh, then the places) for the
// nn-th of the crowd's archetypes (arena/crowd.dta; Crowd::FinishLoading, GH1
// 0x171558). A card is crowd_flat_height high, half that wide and about its
// place (FormFlatCrowd, GH1 0x170160).

#include "gh1/crowd.h"

#include "gh1/rig.h"
#include "gh1/scene.h"

#include <algorithm>
#include <cmath>
#include <string>

namespace gh2::gh1
{
    namespace
    {
        using milo::putStr;
        using milo::putU32;
        using milo::str;
        using milo::u32;

        // Whether (x, y) is over a face of the Mesh at `faces`, its verts at
        // `verts`, in the mesh's own space (PointInXY, GH1 0x1e3600).
        bool over(const Bytes &mesh, size_t verts, size_t count, size_t faces, size_t faceCount, float x, float y)
        {
            for (size_t f = 0; f < faceCount; ++f)
            {
                float px[3], py[3];
                for (size_t c = 0; c < 3u; ++c)
                {
                    const size_t at = faces + f * 6u + c * 2u;
                    const size_t v = mesh[at] | (mesh[at + 1u] << 8);
                    if (v >= count)
                        return false;
                    px[c] = f32(mesh, verts + v * 48u);
                    py[c] = f32(mesh, verts + v * 48u + 4u);
                }
                const auto side = [&](size_t a, size_t b)
                { return (x - px[a]) * (py[b] - py[a]) - (y - py[a]) * (px[b] - px[a]); };
                const float s0 = side(0, 1), s1 = side(1, 2), s2 = side(2, 0);
                if ((s0 >= 0.0f && s1 >= 0.0f && s2 >= 0.0f) || (s0 <= 0.0f && s1 <= 0.0f && s2 <= 0.0f))
                    return true;
            }
            return false;
        }
    }

    std::vector<Bytes> crowdPlaces(const std::vector<const milo::Dir *> &scenes)
    {
        std::vector<Bytes> places;
        for (int nn = 1; nn < 100; ++nn)
        {
            const Bytes *found = object(scenes, "MultiMesh", numbered("Crowd", nn, ".mm"));
            if (!found || u32(*found, 0u) != 0u || u32(*found, 4u) != 1u)
                break;
            size_t o = 9u;
            names(*found, o);
            o += 16u;
            str(*found, o);
            const size_t end = o + 4u + static_cast<size_t>(u32(*found, o)) * 48u;
            if (end > found->size())
                break;
            places.emplace_back(found->begin() + static_cast<std::ptrdiff_t>(o),
                                found->begin() + static_cast<std::ptrdiff_t>(end));
        }
        return places;
    }

    // A region is crowd_limits<nn>.mesh, a Mesh 25 (its Trans 8, Draw 1,
    // material, owner, nine bytes, then its verts, 48 bytes each, and
    // faces): the places that are over one of its faces and less than a
    // card's height above it, in the mesh's own space, as many as the crowd
    // has members to draw whole (Crowd::InitRegion, GH1 0x170da8). A shot
    // names its region, and the flat cards there give way to those members
    // (Crowd::SwitchRegion, GH1 0x1727b0). Its sphere is about the middle of
    // the box those places are in, its radius the box's diagonal (GH1
    // 0x171378: vsqrt of the corners' difference dotted with itself).
    std::vector<Region> crowdRegions(const std::vector<const milo::Dir *> &scenes, const std::vector<Bytes> &places,
                                     float height, size_t whole)
    {
        std::vector<Region> out;
        for (int nn = 0; nn < 100; ++nn)
        {
            const Bytes *mesh = object(scenes, "Mesh", numbered("crowd_limits", nn, ".mesh"));
            if (!mesh)
                break;
            out.emplace_back();
            if (u32(*mesh, 0u) != 25u || u32(*mesh, 4u) != 8u || mesh->size() < 104u)
                continue;
            // Its world, inverted: a place in the mesh's own space.
            const Xfm local = inverse(xfm(*mesh, 56u));
            size_t o = transEnd(*mesh, 4u) + 5u;
            names(*mesh, o);
            o += 16u;
            str(*mesh, o);
            str(*mesh, o);
            o += 9u;
            const size_t verts = o + 4u, count = u32(*mesh, o);
            const size_t faces = verts + count * 48u + 4u, faceCount = u32(*mesh, verts + count * 48u);
            if (faces + faceCount * 6u > mesh->size())
                continue;
            Region &region = out.back();
            float low[3] = {0.0f, 0.0f, 0.0f}, high[3] = {0.0f, 0.0f, 0.0f};
            for (size_t c = 0; c < places.size(); ++c)
                for (uint32_t k = 0; k < u32(places[c], 0u) && region.members.size() < whole; ++k)
                {
                    const size_t at = 4u + static_cast<size_t>(k) * 48u + 36u;
                    const float p[3] = {f32(places[c], at), f32(places[c], at + 4u), f32(places[c], at + 8u)};
                    float q[3];
                    for (size_t j = 0; j < 3u; ++j)
                        q[j] = p[0] * local.m[j] + p[1] * local.m[3u + j] + p[2] * local.m[6u + j] + local.v[j];
                    if (!(q[2] > 0.0f && q[2] < height && over(*mesh, verts, count, faces, faceCount, q[0], q[1])))
                        continue;
                    for (size_t i = 0; i < 3u; ++i)
                    {
                        low[i] = region.members.empty() ? p[i] : std::min(low[i], p[i]);
                        high[i] = region.members.empty() ? p[i] : std::max(high[i], p[i]);
                    }
                    region.members.emplace_back(static_cast<uint32_t>(c), k);
                }
            for (size_t i = 0; i < 3u; ++i)
                region.centre[i] = (low[i] + high[i]) * 0.5f;
            region.radius = std::sqrt((high[0] - low[0]) * (high[0] - low[0]) + (high[1] - low[1]) * (high[1] - low[1]) +
                                      (high[2] - low[2]) * (high[2] - low[2]));
        }
        return out;
    }

    // GH2's WorldCrowd 6 (WorldCrowd::Load, 0x26c430) keeps the same as
    // GH1's: a Draw 3, the mesh it was placed over, how many, a flag, each
    // member's Character with its card's height, density and radius, then
    // each one's places, and its Hmx::Object. Its card is as GH1's
    // (BuildBillboard, 0x26bba0). So `crowd`, which the shots show
    // (gh1/cameras.cpp), has GH1's places and height, with the stand-in's
    // crowd members for GH1's, male before female as GH1 lists them, and any
    // other WorldCrowd has none.
    //
    // The stamp is the first word of what follows its places: a shot's
    // members are kept only if it has the same (CamShot::Load, 0x265508).
    uint32_t crowd(milo::Dir &chars, const std::vector<Bytes> &places, float height)
    {
        uint32_t stamp = 0xffffffffu;
        std::vector<std::string> all;
        for (const auto &[cls, name] : chars.entries)
            if (cls == "Character" && name.rfind("crowd_", 0) == 0)
                all.push_back(name);
        std::sort(all.begin(), all.end(),
                  [](const std::string &l, const std::string &r)
                  {
                      const bool lm = l.find("female") == std::string::npos, rm = r.find("female") == std::string::npos;
                      return lm != rm ? lm : l < r;
                  });
        for (size_t i = 0; i < chars.entries.size(); ++i)
        {
            if (chars.entries[i].first != "WorldCrowd")
                continue;
            const Bytes &b = chars.bodies[i];
            if (u32(b, 0u) != 6u || u32(b, 4u) != 3u)
                continue;
            size_t o = 29u;
            str(b, o);
            const size_t head = o;
            o += 5u;
            const uint32_t count = u32(b, o);
            o += 4u;
            Bytes card;
            for (uint32_t c = 0; c < count && o < b.size(); ++c)
            {
                str(b, o);
                if (o + 12u > b.size())
                    break;
                card.assign(b.begin() + static_cast<std::ptrdiff_t>(o + 4u), b.begin() + static_cast<std::ptrdiff_t>(o + 12u));
                o += 12u;
            }
            for (uint32_t c = 0; c < count && o < b.size(); ++c)
                o += 4u + static_cast<size_t>(u32(b, o)) * 48u;
            if (o > b.size() || card.empty())
                continue;
            // Every member of any of the stand-in's crowds is this one's:
            // one that no crowd has is drawn where it was made.
            const bool shown = chars.entries[i].second == "crowd";
            const size_t listed = shown ? all.size() : 0u;
            uint32_t total = 0u;
            for (size_t c = 0; c < listed && c < places.size(); ++c)
                total += u32(places[c], 0u);
            Bytes out(b.begin(), b.begin() + static_cast<std::ptrdiff_t>(head));
            out[8] = shown ? 1u : 0u;
            putU32(out, total);
            out.push_back(b[head + 4u]);
            putU32(out, static_cast<uint32_t>(listed));
            for (size_t c = 0; c < listed; ++c)
            {
                putStr(out, all[c]);
                putF32(out, height);
                out.insert(out.end(), card.begin(), card.end());
            }
            for (size_t c = 0; c < listed; ++c)
                if (c < places.size())
                    out.insert(out.end(), places[c].begin(), places[c].end());
                else
                    putU32(out, 0u);
            if (shown)
                stamp = u32(b, o);
            put(out, b, o, b.size());
            chars.bodies[i] = std::move(out);
        }
        return stamp;
    }
}
