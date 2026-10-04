// GH1's venues in GH2's.
//
// GH2 keeps a venue as a chain of dirs, each the next one's parent:
// world/<v>/gen/<v>.milo (the camera shots), og/gen/<v>_lighting.milo,
// gen/<v>_chars.milo (the band, its waypoints and the crowd) and
// og/gen/<v>_geom.milo (the room). GH1 keeps one as flat scenes:
// venues/<v>/gen/<v>.rnd (the room), lighting.rnd, campaths.rnd, crowd.rnd.
//
// The geom dir here is GH2's own object, cameras, environs and lights, which
// the dirs above name, around GH1's room: its Tex 8, Mat 21, Mesh 25, Light
// 3 and Environ 1 objects, which GH2's loaders still read, and its Views as
// Groups. The chars dir is GH2's with its waypoints on GH1's spots, and the
// lighting dir GH2's with its spotlights and fixtures unshown. The camera
// shots are GH1's (gh1/cameras.cpp); the presets that light the band, and
// the crowd, are GH2's still.

#include "gh1/venues.h"

#include "disc/ark.h"
#include "gh1/cameras.h"
#include "gh1/rig.h"
#include "gh1/songs.h"
#include "milo/milo.h"

#include <algorithm>
#include <functional>
#include <iostream>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace gh2::gh1
{
    namespace
    {
        using milo::Bytes;
        using milo::putStr;
        using milo::putU32;
        using milo::str;
        using milo::u32;

        // The end of a DataArray at `o` (DataArray::Load, 0x2b0d88: a u16
        // size, line and id, then its nodes), or none for a node not known.
        std::optional<size_t> arrayEnd(const Bytes &b, size_t o)
        {
            if (o + 6u > b.size())
                return std::nullopt;
            const uint32_t size = b[o] | (b[o + 1u] << 8);
            o += 6u;
            for (uint32_t i = 0; i < size; ++i)
            {
                const uint32_t type = u32(b, o);
                o += 4u;
                if (type == 0x10u || type == 0x11u || type == 0x13u)
                {
                    const auto end = arrayEnd(b, o);
                    if (!end)
                        return std::nullopt;
                    o = *end;
                }
                else if (type == 0u || type == 1u)
                    o += 4u;
                else if (type == 4u || type == 5u || type == 0x12u)
                    str(b, o);
                else
                    return std::nullopt;
                if (o > b.size())
                    return std::nullopt;
            }
            return o;
        }

        // Where Hmx::Object's header at `o` ends (0x2c2018): a revision, the
        // type, and TypeProps::Load's flag and array.
        std::optional<size_t> headerEnd(const Bytes &b, size_t o)
        {
            o += 4u;
            str(b, o);
            if (o >= b.size())
                return std::nullopt;
            return b[o] != 0u ? arrayEnd(b, o + 1u) : std::optional<size_t>(o + 1u);
        }

        // A GH2 drawable left out of drawing, by its Draw 3's showing flag:
        // a Mesh 28's or Group 12's after its Trans 9 (a Group's Anim 4 comes
        // first), a Spotlight 20's (Spotlight::Load, 0x273840) before it.
        void hide(const std::string &cls, Bytes &b)
        {
            auto o = headerEnd(b, 4u);
            if (!o)
                return;
            if (cls == "Group" && u32(b, *o) == 4u)
                *o += 12u;
            if (cls != "Spotlight" && u32(b, *o) == 9u)
                *o = transEnd(b, *o);
            if (u32(b, *o) == 3u && *o + 4u < b.size())
                b[*o + 4u] = 0u;
        }

        // A dir's own object less the properties that name its objects: the
        // dir's revisions, then Hmx::Object's header (0x2c2018: a revision,
        // the type, and TypeProps::Load's flag and array of keys and
        // values). An object or a list of them goes, for the type's default
        // (world_objects.dta: no object, an empty list).
        std::optional<Bytes> withoutObjects(const Bytes &root)
        {
            size_t o = 12u;
            str(root, o);
            const size_t flag = o;
            if (flag >= root.size())
                return std::nullopt;
            if (root[flag] == 0u)
                return root;
            const auto end = arrayEnd(root, flag + 1u);
            if (!end)
                return std::nullopt;
            Bytes kept;
            uint32_t count = 0u;
            o = flag + 7u;
            while (o < *end)
            {
                const size_t key = o;
                o += 4u;
                str(root, o);
                const uint32_t type = u32(root, o);
                o += 4u;
                if (type == 0x10u)
                    o = *arrayEnd(root, o);
                else if (type == 0u || type == 1u)
                    o += 4u;
                else
                    str(root, o);
                if (u32(root, key) != 5u || type == 0x10u || type == 4u)
                    continue;
                kept.insert(kept.end(), root.begin() + static_cast<std::ptrdiff_t>(key),
                            root.begin() + static_cast<std::ptrdiff_t>(o));
                count += 2u;
            }
            Bytes out(root.begin(), root.begin() + static_cast<std::ptrdiff_t>(flag));
            out.push_back(count != 0u ? 1u : 0u);
            if (count != 0u)
            {
                out.push_back(static_cast<uint8_t>(count));
                out.push_back(static_cast<uint8_t>(count >> 8));
                out.insert(out.end(), 4u, 0u);
                out.insert(out.end(), kept.begin(), kept.end());
            }
            out.insert(out.end(), root.begin() + static_cast<std::ptrdiff_t>(*end), root.end());
            return out;
        }

        // A count, then that many names.
        std::vector<std::string> names(const Bytes &b, size_t &o)
        {
            std::vector<std::string> out;
            const uint32_t n = u32(b, o);
            o += 4u;
            for (uint32_t i = 0; i < n && o < b.size(); ++i)
                out.push_back(str(b, o));
            return out;
        }

        void putNames(Bytes &out, const std::vector<std::string> &names)
        {
            putU32(out, static_cast<uint32_t>(names.size()));
            for (const std::string &n : names)
                putStr(out, n);
        }

        // Where the children of a View 7's Anim 0 start: past its revision
        // and filters (RndAnimatable::Load, 0x1ab228: a kind and two floats,
        // a flag more for kind 1 and a float more for kind 4). Its Trans 8
        // follows them.
        std::optional<size_t> viewAnims(const Bytes &view)
        {
            if (u32(view, 0u) != 7u || u32(view, 4u) != 0u)
                return std::nullopt;
            size_t o = 12u;
            for (uint32_t i = 0, n = u32(view, 8u); i < n; ++i)
            {
                const uint32_t kind = u32(view, o);
                if (kind > 4u)
                    return std::nullopt;
                o += 12u + (kind == 1u ? 1u : kind == 4u ? 4u : 0u);
            }
            return o;
        }

        // Into a Trans 8, its children: past its rev, local and world.
        constexpr size_t kChildren = 100u;

        // A GH1 scene as GH2 draws it.
        //
        // GH1 draws a scene from its top View down: each View its Draw 1's
        // children in order, each Mesh itself and then its own. GH2's dir
        // draws every drawable no Group lists (RndDir::SyncObjects,
        // 0x1b2f78), so what GH1's tree leaves out is hidden here.
        //
        // A View 7 is a Group to GH2: RndGroup::Load (0x3d5330) takes
        // revisions under 8, and RndDrawable::Load (0x3d5090) adds a Draw
        // 1's children to the group it is loading (an Environ among them as
        // the group's own, a Cam not at all). It drops a Mesh's, so each
        // View lists them here after their Mesh.
        //
        // RndTransformable::Load (0x3d72d0) parents each of a Trans 8's
        // children without a check that it is there, so each list keeps
        // only those that are.
        struct Object
        {
            std::string cls, name;
            Bytes body;
        };

        // The objects of `scene` in those classes, less the names in
        // `without`, drawn from `top` but for the names in `hidden`.
        std::vector<Object> objects(const milo::Dir &scene, const std::set<std::string> &classes, const std::string &top,
                                    const std::set<std::string> &without, const std::set<std::string> &hidden)
        {
            std::map<std::string, size_t> trans;                   // by Mesh, View or Light, where its Trans 8 is
            std::map<std::string, std::vector<std::string>> draws; // by Mesh or View, its Draw 1's children
            std::set<std::string> meshes;
            for (size_t i = 0; i < scene.entries.size(); ++i)
            {
                const auto &[c, n] = scene.entries[i];
                const Bytes &b = scene.bodies[i];
                if (!classes.count(c) || without.count(n))
                    continue;
                auto at = c == "Mesh" || c == "Light" ? std::optional<size_t>(4u) : c == "View" ? viewAnims(b) : std::nullopt;
                if (at && c == "View")
                    names(b, *at);
                if (!at || u32(b, *at) != 8u)
                    continue;
                trans[n] = *at;
                if (c == "Light")
                    continue;
                size_t o = transEnd(b, *at) + 5u;
                draws[n] = names(b, o);
                if (c == "Mesh")
                    meshes.insert(n);
            }

            // A View's list: its children, each Mesh's own after it.
            const auto listed = [&](const std::string &view)
            {
                std::vector<std::string> out;
                const std::function<void(const std::string &, int)> add = [&](const std::string &n, int depth)
                {
                    out.push_back(n);
                    if (meshes.count(n) && depth < 8)
                        for (const std::string &child : draws[n])
                            add(child, depth + 1);
                };
                for (const std::string &child : draws[view])
                    add(child, 0);
                return out;
            };
            std::set<std::string> drawn;
            const std::function<void(const std::string &)> reach = [&](const std::string &n)
            {
                if (draws.count(n) && !hidden.count(n) && drawn.insert(n).second)
                    for (const std::string &child : draws[n])
                        reach(child);
            };
            reach(top);

            std::vector<Object> out;
            for (size_t i = 0; i < scene.entries.size(); ++i)
            {
                const auto &[c, n] = scene.entries[i];
                const Bytes &b = scene.bodies[i];
                if (!classes.count(c) || without.count(n))
                    continue;
                const auto at = trans.find(n);
                if (at == trans.end())
                {
                    if (c == "Tex" || c == "Mat" || c == "Environ")
                        out.push_back({c, n, b});
                    continue;
                }
                size_t o = at->second + kChildren;
                Bytes body;
                if (c == "View")
                {
                    // Its Anim 0's children join the group too, which draws
                    // every drawable in it (RndGroup::Update, 0x1ba308): one
                    // this View animates and another draws stays out.
                    size_t anims = *viewAnims(b);
                    body.assign(b.begin(), b.begin() + static_cast<std::ptrdiff_t>(anims));
                    std::vector<std::string> animated = names(b, anims);
                    const std::vector<std::string> list = listed(n);
                    std::erase_if(animated, [&](const std::string &child)
                                  { return draws.count(child) != 0u && std::find(list.begin(), list.end(), child) == list.end(); });
                    putNames(body, animated);
                    body.insert(body.end(), b.begin() + static_cast<std::ptrdiff_t>(at->second),
                                b.begin() + static_cast<std::ptrdiff_t>(o));
                }
                else
                    body.assign(b.begin(), b.begin() + static_cast<std::ptrdiff_t>(o));
                std::vector<std::string> children = names(b, o);
                std::erase_if(children, [&](const std::string &child) { return trans.count(child) == 0u; });
                putNames(body, children);
                // The constraint, its target, preserve scale, then the
                // parent: the object's own name for none, as here when the
                // one it names is left out.
                size_t parent = o + 4u;
                str(b, parent);
                parent += 1u;
                body.insert(body.end(), b.begin() + static_cast<std::ptrdiff_t>(o),
                            b.begin() + static_cast<std::ptrdiff_t>(parent));
                const size_t draw = transEnd(b, at->second);
                const std::string named = str(b, parent);
                putStr(body, trans.count(named) ? named : n);
                if (c == "Light")
                {
                    body.insert(body.end(), b.begin() + static_cast<std::ptrdiff_t>(draw), b.end());
                    out.push_back({c, n, std::move(body)});
                    continue;
                }
                // Draw 1: a revision, the showing flag, the children, a sphere.
                body.insert(body.end(), b.begin() + static_cast<std::ptrdiff_t>(draw),
                            b.begin() + static_cast<std::ptrdiff_t>(draw + 4u));
                body.push_back(drawn.count(n) ? b[draw + 4u] : 0u);
                if (c == "Mesh")
                    body.insert(body.end(), b.begin() + static_cast<std::ptrdiff_t>(draw + 5u), b.end());
                else
                {
                    putNames(body, listed(n));
                    o = draw + 5u;
                    names(b, o);
                    body.insert(body.end(), b.begin() + static_cast<std::ptrdiff_t>(o),
                                b.begin() + static_cast<std::ptrdiff_t>(std::min(o + 16u, b.size())));
                }
                out.push_back({c == "View" ? "Group" : c, n, std::move(body)});
            }
            return out;
        }

        // GH2's geom dir around GH1's room: its own object, and the cameras,
        // environs and lights the dirs above it name. GH1's environs and
        // lights come with the room, and its lighting scene's beside them.
        // GH1's drum kit is part of the room and GH2's of its drummer, who
        // plays here, so GH1's `kit` stays out.
        std::optional<milo::Dir> geom(const milo::Dir &gh2, const milo::Dir &room, const milo::Dir &lighting,
                                      const std::string &kit)
        {
            const auto root = withoutObjects(gh2.root);
            if (!root)
                return std::nullopt;
            milo::Dir out = gh2;
            out.root = *root;
            out.entries.clear();
            out.bodies.clear();
            static const std::set<std::string> kKept = {"Cam", "Environ", "Light"};
            for (size_t i = 0; i < gh2.entries.size(); ++i)
                if (kKept.count(gh2.entries[i].first))
                {
                    out.entries.push_back(gh2.entries[i]);
                    out.bodies.push_back(gh2.bodies[i]);
                }
            std::set<std::string> taken;
            for (const auto &e : out.entries)
                taken.insert(e.second);
            for (Object &o : objects(room, {"Tex", "Mat", "Mesh", "View", "Light", "Environ"}, "venue.view", taken, {kit}))
            {
                taken.insert(o.name);
                milo::add(out, o.cls, o.name, std::move(o.body));
            }
            for (Object &o : objects(lighting, {"Light", "Environ"}, "", taken, {}))
                milo::add(out, o.cls, o.name, std::move(o.body));
            return out;
        }

        // GH2's lighting dir with what it draws left out: the spotlights
        // and fixtures of GH2's room. Its presets still light the band.
        milo::Dir lights(const milo::Dir &gh2)
        {
            milo::Dir out = gh2;
            for (size_t i = 0; i < out.entries.size(); ++i)
                if (const std::string &c = out.entries[i].first; c == "Mesh" || c == "Group" || c == "Spotlight")
                    hide(c, out.bodies[i]);
            return out;
        }

        // GH1's spots, each a Mesh's world: where its band stands
        // (stage_spot_NN.mesh; charsys.dta's band_spots has the singer and
        // keyboard on 01, the bass on 02, the drummer on 03) and where its
        // guitarist starts and walks to (walk_spot_NN.mesh).
        struct Spots
        {
            std::vector<Bytes> stage, walk;
        };

        Spots spots(const std::vector<const milo::Dir *> &scenes)
        {
            const auto world = [&](const std::string &name) -> std::optional<Bytes>
            {
                for (const milo::Dir *scene : scenes)
                    for (size_t i = 0; i < scene->entries.size(); ++i)
                        if (scene->entries[i] == std::pair<std::string, std::string>("Mesh", name) &&
                            scene->bodies[i].size() >= 104u)
                            return Bytes(scene->bodies[i].begin() + 56, scene->bodies[i].begin() + 104);
                return std::nullopt;
            };
            Spots out;
            for (int i = 1; auto spot = world("stage_spot_0" + std::to_string(i) + ".mesh"); ++i)
                out.stage.push_back(std::move(*spot));
            for (int i = 1; auto spot = world("walk_spot_0" + std::to_string(i) + ".mesh"); ++i)
                out.walk.push_back(std::move(*spot));
            return out;
        }

        // Where a Waypoint 3's Trans 9 starts (Waypoint::Load, 0x192068):
        // past Hmx::Object's header and a Draw 3. Its flags follow it.
        std::optional<size_t> waypointTrans(const Bytes &b)
        {
            const auto o = headerEnd(b, 4u);
            if (!o || u32(b, 0u) != 3u || u32(b, *o) != 3u || u32(b, *o + 25u) != 9u)
                return std::nullopt;
            return *o + 25u;
        }

        // GH2's chars dir with its waypoints on GH1's spots, each by its
        // flags (macros.dta): the band's starts on GH1's, the guitarist's on
        // walk spot 01, and the waypoints it walks to on the rest, a solo's
        // first. GH1 has no second guitarist: the two start on 02 and 01.
        // `onWalk` gets the names of those put on each walk spot.
        std::optional<milo::Dir> chars(const milo::Dir &gh2, const Spots &at, std::vector<std::vector<std::string>> &onWalk)
        {
            if (at.stage.size() < 3u || at.walk.empty())
                return std::nullopt;
            enum : uint32_t
            {
                kStartGuitarist0 = 1u,
                kStartGuitarist1Mp = 2u,
                kStartSinger = 4u,
                kStartKeyboardist = 8u,
                kStartBassist = 16u,
                kStartDrummer = 32u,
                kSoloWalkSpot = 128u,
                kStartGuitarist0Mp = 512u,
            };
            milo::Dir out = gh2;
            onWalk.assign(at.walk.size(), {});
            std::vector<std::pair<size_t, size_t>> walks; // entry, its Trans
            for (size_t i = 0; i < out.entries.size(); ++i)
            {
                if (out.entries[i].first != "Waypoint")
                    continue;
                Bytes &b = out.bodies[i];
                const auto trans = waypointTrans(b);
                if (!trans)
                    return std::nullopt;
                const uint32_t flags = u32(b, transEnd(b, *trans));
                const Bytes *spot = nullptr;
                if (flags & kStartGuitarist0)
                {
                    spot = &at.walk[0];
                    onWalk[0].push_back(out.entries[i].second);
                }
                else if (flags & (kStartSinger | kStartKeyboardist))
                    spot = &at.stage[0];
                else if (flags & kStartBassist)
                    spot = &at.stage[1];
                else if (flags & kStartDrummer)
                    spot = &at.stage[2];
                else if (flags & kStartGuitarist0Mp)
                    spot = &at.walk[at.walk.size() > 1u ? 1u : 0u];
                else if (flags & kStartGuitarist1Mp)
                    spot = &at.walk[0];
                else if (flags & kSoloWalkSpot)
                    walks.insert(walks.begin(), {i, *trans});
                else
                    walks.emplace_back(i, *trans);
                if (spot)
                    for (const size_t o : {*trans + 4u, *trans + 52u})
                        std::copy(spot->begin(), spot->end(), b.begin() + static_cast<std::ptrdiff_t>(o));
            }
            for (size_t w = 0; w < walks.size(); ++w)
            {
                const size_t on = at.walk.size() > 1u ? 1u + w % (at.walk.size() - 1u) : 0u;
                const Bytes &spot = at.walk[on];
                onWalk[on].push_back(out.entries[walks[w].first].second);
                for (const size_t o : {walks[w].second + 4u, walks[w].second + 52u})
                    std::copy(spot.begin(), spot.end(),
                              out.bodies[walks[w].first].begin() + static_cast<std::ptrdiff_t>(o));
            }
            return out;
        }
    }

    void addVenues(size_t layer, size_t disc)
    {
        // GH1's venues, each with the View that is its drum kit.
        static const std::pair<const char *, const char *> kVenues[] = {
            {"basement", "drum_kit.view"}, {"small_club", "drumkit.view"}, {"big_club", "drum_kit.view"},
            {"theatre", "drum_kit.view"},  {"fest", "drum_kit.view"},      {"arena", "drum kit 00.view"},
        };
        for (const auto &[name, kit] : kVenues)
        {
            const std::string ours = venue(name);
            const std::string world = "world/" + ours + "/", theirs = std::string("venues/") + name + "/gen/";
            const std::string geomPath = world + "og/gen/" + ours + "_geom.milo_ps2";
            const std::string lightsPath = world + "og/gen/" + ours + "_lighting.milo_ps2";
            const std::string charsPath = world + "gen/" + ours + "_chars.milo_ps2";
            const auto gh2Geom = load(0u, geomPath);
            const auto gh2Lights = load(0u, lightsPath);
            const auto gh2Chars = load(0u, charsPath);
            const auto room = load(disc, theirs + name + ".rnd_ps2");
            const auto lighting = load(disc, theirs + "lighting.rnd_ps2");
            if (!gh2Geom || !gh2Lights || !gh2Chars || !room || !lighting)
            {
                std::cerr << "[gh1] cannot read " << name << "'s venue" << std::endl;
                continue;
            }
            const auto madeGeom = geom(*gh2Geom, *room, *lighting, kit);
            const Spots at = spots({&*lighting, &*room});
            Stage stage;
            const auto madeChars = chars(*gh2Chars, at, stage.walks);
            if (!madeGeom || !madeChars)
            {
                std::cerr << "[gh1] cannot build " << name << "'s venue" << std::endl;
                continue;
            }
            ark::addFile(layer, geomPath, milo::write(*madeGeom));
            ark::addFile(layer, lightsPath, milo::write(lights(*gh2Lights)));
            ark::addFile(layer, charsPath, milo::write(*madeChars));
            stage.spot = at.stage[0];
            addCameras(layer, disc, name, ours, stage);
        }
    }
}
