// What a GH1 screen takes of GH2's scene of the same name.
//
// GH2's script for the screen names objects GH1's scene has no like of.
// Each comes from GH2's scene as it is, for the script to find: a widget
// with a place on a GH1 screen (a list, a proxy, a placer) shows, and what
// is GH2's own look (its labels, buttons and meshes) does not, unless the
// screen keeps it. A Group comes with nothing in it: what it listed is
// GH2's art, or objects GH1's tree draws already.

#include "gh1/screen.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace gh2::gh1
{
    namespace
    {
        using milo::putU32;
        using milo::str;
        using milo::u32;

        // Where a GH2 object's Trans 9 and Draw 3 are. Each starts with its
        // classes' revisions, down to a UIComponent 1 (0x20d490) for a
        // widget, then Hmx::Object's header (0x2c2018); a Group 12 has its
        // Anim 4 next (RndGroup::Load, 0x3d5330), and a BandPlacer 2 its
        // Draw before its Trans (BandPlacer::Load, 0x290f28).
        struct Spots
        {
            size_t trans = 0u, draw = 0u;
        };

        std::optional<Spots> spots(const std::string &cls, const Bytes &b)
        {
            static const std::map<std::string, size_t> kRevisions = {
                {"BandLabel", 3u}, {"BandButton", 4u}, {"BandSlider", 3u},    {"UIList", 2u}, {"UIPicture", 2u}, {"UIProxy", 2u},
                {"CheckBox", 2u},  {"Mesh", 1u},       {"BandTextEntry", 2u}, {"Group", 1u},  {"BandPlacer", 1u},
            };
            const auto revisions = kRevisions.find(cls);
            if (revisions == kRevisions.end())
                return std::nullopt;
            size_t o = 4u * revisions->second + 4u;
            str(b, o);
            if (u8(b, o) != 0u)
                return std::nullopt;
            o += cls == "Group" ? 13u : 1u;
            Spots out;
            if (cls == "BandPlacer")
            {
                out.draw = o;
                out.trans = o + 25u;
            }
            else
            {
                out.trans = o;
                o += 104u;
                str(b, o);
                o += 1u;
                str(b, o);
                out.draw = o;
            }
            if (u32(b, out.trans) != 9u || u32(b, out.draw) != 3u || out.draw + 25u > b.size() ||
                out.trans + 100u > b.size())
                return std::nullopt;
            return out;
        }

        // A Group 12 with nothing listed: its objects follow its Draw 3.
        void empty(Bytes &b, const Spots &at)
        {
            const size_t list = at.draw + 25u;
            size_t o = list;
            names(b, o);
            Bytes out(b.begin(), b.begin() + static_cast<std::ptrdiff_t>(list));
            putU32(out, 0u);
            put(out, b, o, b.size());
            b = std::move(out);
        }
    }

    // GH2's own, each with the material and texture it draws with.
    void Conversion::take(const std::string &name, int depth)
    {
        static const std::set<std::string> kLook = {"BandLabel", "BandButton", "Mesh"};
        const auto i = milo::find(gh2, name);
        if (have.count(name) || depth > 4 || !i)
            return;
        have.insert(name);
        const std::string &c = gh2.entries[*i].first;
        if (c == "Mesh" || c == "Mat" || c == "Environ")
            for (const std::string &needed : named(gh2.bodies[*i], c == "Mesh" ? ".mat" : c == "Mat" ? ".tex" : ".lit"))
                take(needed, depth + 1);
        Bytes body = gh2.bodies[*i];
        // GH2's lights light what its placers place, so each moves as far
        // as the placer it stood nearest: a Light 6 is Hmx::Object's
        // header, then a Trans 9, its local's position 40 bytes in and its
        // world's 88.
        if (c == "Light" && !moves.empty())
        {
            size_t o = 8u;
            str(body, o);
            o += 1u;
            if (u32(body, o) == 9u && o + 100u <= body.size())
            {
                const auto distance = [&](const Move &m)
                {
                    float d = 0.0f;
                    for (size_t axis = 0; axis < 3u; ++axis)
                        d += std::pow(f32(body, o + 88u + 4u * axis) - m.from[axis], 2.0f);
                    return d;
                };
                const Move &nearest = *std::min_element(
                    moves.begin(), moves.end(), [&](const Move &a, const Move &b) { return distance(a) < distance(b); });
                for (const size_t at : {o + 40u, o + 88u})
                    for (size_t axis = 0; axis < 3u; ++axis)
                    {
                        const float moved = f32(body, at + 4u * axis) + nearest.by[axis];
                        std::memcpy(body.data() + at + 4u * axis, &moved, 4u);
                    }
            }
        }
        if (const auto at = spots(c, body))
        {
            if (kLook.count(c) && !kept.count(name))
                body[at->draw + 4u] = 0u;
            if (c == "Group" && !kept.count(name))
                empty(body, *at);
            // Its local and its world both: it stands under nothing here.
            for (const Screen::Place &place : screen.placed)
                if (name == place.name)
                    for (const size_t o : {at->trans + 4u, at->trans + 52u})
                    {
                        const float to[3] = {place.x, place.y, place.z};
                        std::memcpy(body.data() + o + 36u, to, 12u);
                        for (size_t f = 0; place.scale != 0.0f && f < 9u; ++f)
                        {
                            const float axis = f % 4u == 0u ? place.scale : 0.0f;
                            std::memcpy(body.data() + o + 4u * f, &axis, 4u);
                        }
                        for (size_t f = 0; place.grown != 0.0f && f < 9u; ++f)
                        {
                            const float axis = f32(body, o + 4u * f) * place.grown;
                            std::memcpy(body.data() + o + 4u * f, &axis, 4u);
                        }
                    }
        }
        milo::add(out, c, name, std::move(body));
    }

    // What GH2's scene fills in, then the filters its script sets frames by.
    void Conversion::takeFromGh2()
    {
        for (const auto &e : out.entries)
            have.insert(e.second);
        for (const Screen::Place &place : screen.placed)
        {
            const auto i = milo::find(gh2, place.name);
            if (const auto at = i && gh2.entries[*i].first == "BandPlacer" ? spots("BandPlacer", gh2.bodies[*i])
                                                                            : std::nullopt)
            {
                const float to[3] = {place.x, place.y, place.z};
                Move move;
                for (size_t axis = 0; axis < 3u; ++axis)
                {
                    move.from[axis] = f32(gh2.bodies[*i], at->trans + 88u + 4u * axis);
                    move.by[axis] = to[axis] - move.from[axis];
                }
                moves.push_back(move);
            }
        }
        for (const std::string &n : filled)
            take(n, 0);
        for (const auto &[n, to] : screen.frames)
            milo::add(out, "AnimFilter", n, filter(to, Anim()));
    }
}
