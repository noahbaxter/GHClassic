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
// 3, Environ 1, Flare 3, ParticleSys 22 and anim objects (TransAnim 4,
// MatAnim 5, LightAnim 1, EnvAnim 3, MeshAnim 0, ParticleSysAnim 2), which GH2's loaders
// still read, and its Views as Groups, with GH1's lighting scene beside it.
// The chars dir is GH2's with its waypoints on GH1's spots, and the lighting
// dir GH2's with its spotlights and fixtures unshown. The camera shots are
// GH1's (gh1/cameras.cpp) and the script that drives the anims GH1's
// (gh1/scripts.cpp); the presets that light the band, and the crowd, are
// GH2's still.

#include "gh1/venues.h"

#include "disc/ark.h"
#include "formats/dtb.h"
#include "gh1/cameras.h"
#include "gh1/rig.h"
#include "gh1/scripts.h"
#include "gh1/songs.h"
#include "milo/milo.h"

#include <algorithm>
#include <cmath>
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

        // An Anim 0 (RndAnimatable::Load, 0x1ab228): its filters, each a
        // kind and two floats (0 a scale and an offset; 1 a range, with a
        // flag more for whether it loops; 4 a float more), then the anims it
        // drives.
        struct Anim
        {
            float scale = 1.0f, offset = 0.0f, min = 0.0f, max = 0.0f;
            bool loop = false;
            std::vector<std::string> children;

            // Whether GH2 makes a filter of it (0x1ab494).
            bool filtered() const { return scale != 1.0f || offset != 0.0f || min != max; }
        };

        // The Anim 0 at `o`, which is left past it.
        std::optional<Anim> anim(const Bytes &b, size_t &o)
        {
            if (u32(b, o) != 0u)
                return std::nullopt;
            Anim out;
            const uint32_t filters = u32(b, o + 4u);
            o += 8u;
            for (uint32_t i = 0; i < filters; ++i)
            {
                const uint32_t kind = u32(b, o);
                if (kind > 4u)
                    return std::nullopt;
                if (kind == 0u)
                {
                    out.scale = f32(b, o + 4u);
                    out.offset = f32(b, o + 8u);
                }
                else if (kind == 1u)
                {
                    out.min = f32(b, o + 4u);
                    out.max = f32(b, o + 8u);
                    out.loop = o + 12u < b.size() && b[o + 12u] != 0u;
                }
                o += 12u + (kind == 1u ? 1u : kind == 4u ? 4u : 0u);
            }
            out.children = names(b, o);
            return out;
        }

        // An Anim 4 (0x1ab2d8) in place of an Anim 0: its frame, and GH1's
        // rate, 480 frames a beat (Arena::Poll, GH1 0x16a020, sets a scene's
        // frame to the song's tick).
        void putTicks(Bytes &out)
        {
            putU32(out, 4u);
            putF32(out, 0.0f);
            putU32(out, 1u);
        }

        // GH2's name for the filter of an anim that has one.
        std::string filterOf(const std::string &name)
        {
            return name + ".filt";
        }

        // An AnimFilter 1 (RndAnimFilter::Load, 0x1acf48) of that anim:
        // Hmx::Object's header, an Anim 4 at 480 frames a beat, the anim,
        // its scale, offset and range, whether it loops, and no period.
        Bytes filter(const std::string &name, const Anim &of)
        {
            Bytes out;
            putU32(out, 1u);
            putU32(out, 0u);
            putStr(out, {});
            out.push_back(0u);
            putU32(out, 4u);
            putF32(out, 0.0f);
            putU32(out, 1u);
            putStr(out, name);
            putF32(out, of.scale);
            putF32(out, of.offset);
            // No range is every frame. A looping one keeps its last frame:
            // small_club's neon1.envanim held at 480 of 0 to 480 is lit in
            // retail and wraps to its dark frame 0 here.
            putF32(out, of.min != of.max ? of.min : -1.0e9f);
            putF32(out, of.min != of.max ? (of.loop ? std::nextafter(of.max, 1.0e9f) : of.max) : 1.0e9f);
            putU32(out, of.loop ? 1u : 0u);
            putF32(out, 0.0f);
            return out;
        }

        // Into a Trans 8, its children: past its rev, local and world.
        constexpr size_t kChildren = 100u;

        // The Views GH1 draws its room and its lighting scene from.
        constexpr const char *kRoom = "venue.view";
        constexpr const char *kLighting = "lighting.view";

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
            // A Mesh no View of GH1's tree reaches: GH1 never draws it.
            bool unreached = false;
        };

        // GH1 sets a scene's frame from its top View down: each anim takes
        // its View's frame through its own filters and hands it to its
        // children (RndAnimatable::SetFrame, GH1 0x1b3d00). GH2's
        // RndAnimatable::Load (0x1ab228) takes an Anim 0's filters out into
        // an AnimFilter that nothing drives, and a Group sets the frame of
        // every anim in it, those it draws too. So an anim's filters are an
        // AnimFilter made here, and a View's anims a Group of their own
        // beside the one it draws, "<view>.anims", unshown: what stands for
        // an anim, to whatever drives it, is its filter or else itself.
        //
        // RndMatAnim::LoadStages (0x1c2c28) hands a stage after the first to
        // an anim nothing drives, of the pass RndMat::LoadStages (0x1bf350)
        // made of that stage, or loads it over the first. Each is an anim
        // of its own here, "<anim>_<n>.mnm" of "<mat>_<n>.mat", driven with
        // the first.
        std::string animsOf(const std::string &view)
        {
            return view + ".anims";
        }

        // A name less its extension.
        std::string base(const std::string &name)
        {
            return name.substr(0, name.rfind('.'));
        }

        // The View a section draws after the band, by the one it draws
        // before.
        std::string transparentOf(const std::string &view)
        {
            return base(view) + "_transparent.view";
        }

        // A View 7 of those anims and draws: its Anim 0, a Trans 8 at the
        // origin under itself, and a Draw 1 showing, as a Group must be to
        // set a frame (RndGroup::SetFrame, 0x1ba8a0).
        Bytes group(const std::string &name, const std::vector<std::string> &anims,
                    const std::vector<std::string> &draws)
        {
            Bytes out;
            putU32(out, 7u);
            putU32(out, 0u);
            putU32(out, 0u);
            putNames(out, anims);
            putU32(out, 8u);
            for (int i = 0; i < 2; ++i)
                for (int f = 0; f < 12; ++f)
                    putF32(out, f == 0 || f == 4 || f == 8 ? 1.0f : 0.0f);
            putU32(out, 0u);
            putU32(out, 0u);
            putStr(out, {});
            out.push_back(0u);
            putStr(out, name);
            putU32(out, 1u);
            out.push_back(1u);
            putNames(out, draws);
            out.insert(out.end(), 16u, 0u);
            return out;
        }

        // An Environ 1 less the children its Draw 1 lists.
        Bytes withoutDraws(const Bytes &b)
        {
            size_t o = 9u;
            names(b, o);
            Bytes out(b.begin(), b.begin() + 9);
            putU32(out, 0u);
            out.insert(out.end(), b.begin() + static_cast<std::ptrdiff_t>(std::min(o, b.size())), b.end());
            return out;
        }

        // A MatAnim 5's stages (RndMatAnim::Load, GH1 0x1cb570): where each
        // starts and the last ends, and whether each has keys. A stage is
        // its translation, scale and rotation keys, each a vector and a
        // frame, then its texture keys, each a name and a frame.
        struct Stages
        {
            size_t count = 0u; // where their count is
            std::vector<size_t> at;
            std::vector<bool> keyed;
        };

        Stages stages(const Bytes &b, size_t o)
        {
            Stages out;
            str(b, o);
            out.count = o;
            const uint32_t n = u32(b, o);
            o += 4u;
            for (uint32_t i = 0; i < n && o < b.size(); ++i)
            {
                out.at.push_back(o);
                bool keyed = false;
                for (int list = 0; list < 3; ++list)
                {
                    keyed |= u32(b, o) != 0u;
                    o += 4u + static_cast<size_t>(u32(b, o)) * 16u;
                }
                const uint32_t textures = u32(b, o);
                keyed |= textures != 0u;
                o += 4u;
                for (uint32_t t = 0; t < textures && o < b.size(); ++t)
                {
                    str(b, o);
                    o += 4u;
                }
                out.keyed.push_back(keyed);
            }
            out.at.push_back(o);
            return out;
        }

        // The objects of `scene` in those classes, less the names in
        // `without`, drawn from `top` but for the names in `hidden`. An anim
        // in `scripted` is left out of every View's, for the script that
        // drives it. `drivers` gets what stands for each anim.
        std::vector<Object> objects(const milo::Dir &scene, const std::set<std::string> &classes, const std::string &top,
                                    const std::set<std::string> &without, const std::set<std::string> &hidden,
                                    const std::set<std::string> &scripted, Drivers &drivers)
        {
            static const std::set<std::string> kAnims = {"TransAnim", "MatAnim",  "LightAnim",
                                                         "EnvAnim",   "MeshAnim", "ParticleSysAnim"};
            struct Parts
            {
                std::optional<Anim> anim;
                size_t rest = 4u; // past its Anim 0
            };
            std::map<std::string, Parts> parts;
            std::map<std::string, size_t> trans;                   // by name, where its Trans 8 is
            std::map<std::string, std::vector<std::string>> draws; // by Mesh or View, its Draw 1's children
            std::map<std::string, std::vector<std::string>> hangs; // by name, its Trans 8's children
            std::map<std::string, std::string> owners;             // by View, its children's owner
            std::set<std::string> meshes, environs;
            std::set<std::string> moved; // what a TransAnim moves
            for (size_t i = 0; i < scene.entries.size(); ++i)
            {
                const auto &[c, n] = scene.entries[i];
                const Bytes &b = scene.bodies[i];
                if (!classes.count(c) || without.count(n))
                    continue;
                if (c == "Environ" && u32(b, 0u) == 1u && u32(b, 4u) == 1u)
                {
                    size_t o = 9u;
                    draws[n] = names(b, o);
                    environs.insert(n);
                }
                Parts p;
                if (c == "View" || c == "ParticleSys" || kAnims.count(c))
                {
                    p.anim = anim(b, p.rest);
                    if (!p.anim)
                        continue;
                    // A TransAnim 4 (RndTransAnim::Load, GH1 0x1dd538): after
                    // its Anim 0 a Draw 1, then the Trans it moves.
                    if (c == "TransAnim" && u32(b, p.rest) == 1u)
                    {
                        size_t o = p.rest + 5u;
                        names(b, o);
                        o += 16u;
                        moved.insert(str(b, o));
                    }
                }
                else if (c != "Mesh" && c != "Light" && c != "Flare")
                    continue;
                if (!kAnims.count(c))
                {
                    if (u32(b, p.rest) != 8u)
                        continue;
                    trans[n] = p.rest;
                    size_t children = p.rest + kChildren;
                    hangs[n] = names(b, children);
                    if (c == "Mesh" || c == "View")
                    {
                        size_t o = transEnd(b, p.rest) + 5u;
                        draws[n] = names(b, o);
                        // Past a View's sphere, the View whose children it has
                        // (RndView::Load, GH1 0x2eb3f0).
                        o += 16u;
                        if (c == "View" && u32(b, 0u) > 3u)
                            owners[n] = str(b, o);
                    }
                    if (c == "Mesh")
                        meshes.insert(n);
                }
                parts[n] = std::move(p);
            }

            // A View with another's children draws those and then its own
            // (RndView::DrawShowing, GH1 0x1ef8f0).
            for (const auto own = draws; const auto &[view, owner] : owners)
                if (const auto it = own.find(owner); owner != view && it != own.end())
                    draws[view].insert(draws[view].begin(), it->second.begin(), it->second.end());

            // GH1 works a world out only from a section's View down, each
            // Trans its children's (VenueSection::Poll, GH1 0x178ce8;
            // RndTransformable::UpdateWorldXfm, GH1 0x1da570): what that
            // never reaches keeps the world it loads with, here its local
            // with no parent. One a TransAnim moves is left as it is.
            std::set<std::string> hung;
            const std::function<void(const std::string &)> hang = [&](const std::string &n)
            {
                if (!trans.count(n) || !hung.insert(n).second)
                    return;
                for (const std::string &child : hangs[n])
                    hang(child);
            };
            hang(top);
            const auto held = [&](const std::string &n) { return !hung.count(n) && !moved.count(n); };

            // What stands for each anim: a View's anims if it has any here,
            // a MatAnim's later stages after it.
            const auto driven = [&](const Anim &of)
            {
                std::vector<std::string> out;
                for (const std::string &child : of.children)
                    if (parts.count(child) && parts[child].anim && !scripted.count(child))
                        out.push_back(child);
                return out;
            };
            std::map<std::string, size_t> entry;
            for (size_t i = 0; i < scene.entries.size(); ++i)
                entry[scene.entries[i].second] = i;
            const std::function<void(const std::string &, int)> stand = [&](const std::string &n, int depth)
            {
                const auto it = parts.find(n);
                if (it == parts.end() || !it->second.anim || drivers.count(n))
                    return;
                const Anim &a = *it->second.anim;
                const size_t i = entry[n];
                const std::string &c = scene.entries[i].first;
                std::vector<std::string> as;
                if (c == "View")
                {
                    bool any = false;
                    for (const std::string &child : driven(a))
                    {
                        if (depth < 16)
                            stand(child, depth + 1);
                        any |= drivers.count(child) && !drivers[child].empty();
                    }
                    if (any)
                        as.push_back(filterOf(n));
                }
                else
                {
                    // A ParticleSys is drawn by whatever Group it is in, so
                    // its filter stands for it filtered or not.
                    as.push_back(a.filtered() || c == "ParticleSys" ? filterOf(n) : n);
                    const Stages s = c == "MatAnim" ? stages(scene.bodies[i], it->second.rest) : Stages();
                    for (size_t k = 1u; k < s.keyed.size(); ++k)
                        if (s.keyed[k])
                        {
                            const std::string stage = base(n) + "_" + std::to_string(k) + ".mnm";
                            as.push_back(a.filtered() ? filterOf(stage) : stage);
                        }
                }
                drivers[n] = std::move(as);
            };
            for (const auto &e : scene.entries)
                stand(e.second, 0);

            // A View's list: its children, each Mesh's and Environ's own
            // after it. GH1 draws a drawable and then its children
            // (RndDrawable::Draw, GH1 0x2ec850), and an Environ drawn is the
            // one in use from then on, past its View's end
            // (RndEnviron::DrawShowing, GH1 0x2b69e0). A Group puts back the
            // one before it (RndGroup::DrawShowing, 0x1bab10), so what a View
            // draws from an Environ on, or after a View that left another in
            // use, is a Group of that Environ's, "<view>.env<n>".
            //
            // A section's "<name>_transparent.view" is taken out of the View
            // that lists it (VenueSection::IsLoaded, GH1 0x178db8) and drawn
            // after the band (ArenaPanel::Draw, GH1 0x10d488). The Environs
            // of the band, the guitarists and the crowd are taken out too
            // (Arena::SetupEnvs, GH1 0x168370).
            const std::string apart = transparentOf(top);
            const auto ofCharacters = [](const std::string &n)
            {
                return n == "stagechar.env" || n == "crowd.env" ||
                       (n.size() == 11u && n.rfind("singer", 0) == 0 && n.substr(7u) == ".env");
            };
            struct Listing
            {
                std::vector<std::string> own;
                std::vector<std::vector<std::string>> runs; // each its Environ first
                std::string exit;                           // the Environ it leaves in use, if it sets one
            };
            std::map<std::string, Listing> listings;
            // An Environ's own children, a Group "<environ>.draws" first in
            // its run: unshown, GH1's Environ draws none of them and is not
            // put in use.
            std::map<std::string, std::vector<std::string>> owned;
            const std::function<const Listing &(const std::string &, int)> listed =
                [&](const std::string &view, int depth) -> const Listing &
            {
                if (const auto it = listings.find(view); it != listings.end())
                    return it->second;
                listings[view];
                Listing l;
                const std::function<void(const std::string &, int, std::vector<std::string> *)> add =
                    [&](const std::string &n, int within, std::vector<std::string> *into)
                {
                    if (n == apart || ofCharacters(n))
                        return;
                    if (environs.count(n) && !into)
                    {
                        l.runs.push_back({n});
                        l.exit = n;
                        if (!draws[n].empty() && !owned.count(n))
                        {
                            std::vector<std::string> &mine = owned[n];
                            for (const std::string &child : draws[n])
                                add(child, within + 1, &mine);
                            l.runs.back().push_back(drawsOf(n));
                        }
                        return;
                    }
                    (into ? *into : l.runs.empty() ? l.own : l.runs.back()).push_back(n);
                    if (meshes.count(n) && within < 8)
                        for (const std::string &child : draws[n])
                            add(child, within + 1, into);
                    else if (!into && draws.count(n) && !meshes.count(n) && !environs.count(n) && depth < 16)
                        if (const std::string left = listed(n, depth + 1).exit; !left.empty() && left != l.exit)
                        {
                            l.runs.push_back({left});
                            l.exit = left;
                        }
                };
                for (const std::string &child : draws[view])
                    add(child, 0, nullptr);
                return listings[view] = std::move(l);
            };
            // What the tree draws: a Mesh or View reached, and what each lists.
            std::set<std::string> drawn;
            const std::function<void(const std::string &)> reach = [&](const std::string &n)
            {
                if (hidden.count(n) || !drawn.insert(n).second)
                    return;
                if (draws.count(n))
                    for (const std::string &child : draws[n])
                        reach(child);
            };
            reach(top);
            reach(apart);

            std::vector<Object> out;
            for (size_t i = 0; i < scene.entries.size(); ++i)
            {
                const auto &[c, n] = scene.entries[i];
                const Bytes &b = scene.bodies[i];
                if (!classes.count(c) || without.count(n))
                    continue;
                const auto part = parts.find(n);
                if (part == parts.end())
                {
                    if (c == "Tex" || c == "Mat")
                        out.push_back({c, n, b});
                    else if (c == "Environ")
                        out.push_back({c, n, environs.count(n) ? withoutDraws(b) : b});
                    continue;
                }
                const Parts &p = part->second;
                Bytes body(b.begin(), b.begin() + 4);
                if (p.anim)
                {
                    std::vector<std::string> anims;
                    for (const std::string &child : driven(*p.anim))
                        anims.insert(anims.end(), drivers[child].begin(), drivers[child].end());
                    if (c == "View")
                    {
                        // A View draws; its anims are their own Group's.
                        putU32(body, 0u);
                        putU32(body, 0u);
                        putU32(body, 0u);
                        if (!anims.empty())
                        {
                            out.push_back({"Group", animsOf(n), group(animsOf(n), anims, {})});
                            out.push_back({"AnimFilter", filterOf(n), filter(animsOf(n), *p.anim)});
                        }
                    }
                    else
                    {
                        putTicks(body);
                        if (p.anim->filtered() || c == "ParticleSys")
                            out.push_back({"AnimFilter", filterOf(n), filter(n, *p.anim)});
                    }
                }
                if (c == "MatAnim")
                {
                    // Its first stage alone, or none with no material, and
                    // each later one that has keys as an anim of its own.
                    const Stages s = stages(b, p.rest);
                    size_t o = p.rest;
                    const std::string mat = str(b, o);
                    putStr(body, mat);
                    const bool first = !mat.empty() && !s.keyed.empty();
                    putU32(body, first ? 1u : 0u);
                    if (first)
                        body.insert(body.end(), b.begin() + static_cast<std::ptrdiff_t>(s.at[0]),
                                    b.begin() + static_cast<std::ptrdiff_t>(s.at[1]));
                    body.insert(body.end(), b.begin() + static_cast<std::ptrdiff_t>(s.at.back()), b.end());
                    out.push_back({c, n, std::move(body)});
                    for (size_t k = 1u; k < s.keyed.size(); ++k)
                    {
                        if (!s.keyed[k])
                            continue;
                        const std::string stage = base(n) + "_" + std::to_string(k) + ".mnm";
                        Bytes made(b.begin(), b.begin() + 4);
                        putTicks(made);
                        putStr(made, base(mat) + "_" + std::to_string(k) + ".mat");
                        putU32(made, 1u);
                        made.insert(made.end(), b.begin() + static_cast<std::ptrdiff_t>(s.at[k]),
                                    b.begin() + static_cast<std::ptrdiff_t>(s.at[k + 1u]));
                        putStr(made, {});
                        putU32(made, 0u);
                        putU32(made, 0u);
                        out.push_back({c, stage, std::move(made)});
                        if (p.anim->filtered())
                            out.push_back({"AnimFilter", filterOf(stage), filter(stage, *p.anim)});
                    }
                    continue;
                }
                const auto at = trans.find(n);
                if (at == trans.end())
                {
                    body.insert(body.end(), b.begin() + static_cast<std::ptrdiff_t>(p.rest), b.end());
                    out.push_back({c, n, std::move(body)});
                    continue;
                }
                size_t o = at->second + kChildren;
                // Its rev, local and world: the world for both of one held.
                const size_t local = at->second + 4u, world = at->second + 52u;
                const auto span = [&](size_t from, size_t to)
                {
                    body.insert(body.end(), b.begin() + static_cast<std::ptrdiff_t>(from),
                                b.begin() + static_cast<std::ptrdiff_t>(to));
                };
                span(at->second, local);
                span(held(n) ? world : local, held(n) ? o : world);
                span(world, o);
                std::vector<std::string> children = names(b, o);
                std::erase_if(children, [&](const std::string &child) { return trans.count(child) == 0u || held(child); });
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
                if (c != "View")
                    body.insert(body.end(), b.begin() + static_cast<std::ptrdiff_t>(draw + 5u), b.end());
                else
                {
                    const Listing &l = listed(n, 0);
                    std::vector<std::string> list = l.own;
                    for (size_t k = 0; k < l.runs.size(); ++k)
                    {
                        list.push_back(n + ".env" + std::to_string(k + 1u));
                        out.push_back({"Group", list.back(), group(list.back(), {}, l.runs[k])});
                    }
                    putNames(body, list);
                    o = draw + 5u;
                    names(b, o);
                    body.insert(body.end(), b.begin() + static_cast<std::ptrdiff_t>(o),
                                b.begin() + static_cast<std::ptrdiff_t>(std::min(o + 16u, b.size())));
                }
                out.push_back({c == "View" ? "Group" : c, n, std::move(body), c == "Mesh" && !drawn.count(n)});
            }
            for (const auto &[of, list] : owned)
                out.push_back({"Group", drawsOf(of), group(drawsOf(of), {}, list)});
            return out;
        }

        // Whether a body of `dir` holds `name` as a string.
        bool names(const milo::Dir &dir, const std::string &name)
        {
            Bytes key;
            putStr(key, name);
            for (const Bytes &b : dir.bodies)
                if (std::search(b.begin(), b.end(), key.begin(), key.end()) != b.end())
                    return true;
            return false;
        }

        // GH2's geom dir around GH1's room: its own object, and the cameras,
        // environs and lights the dirs above it name, with the materials
        // those dirs' meshes take from it (big_chars' crowd_plane.mesh has
        // ray_blocker.mat) and their textures. GH1's environs and lights
        // come with the room, and its lighting scene's beside them.
        // `unreached` gets the meshes GH1 never draws, which no script is
        // to show (fest's shows solo_beam01.mesh, GH2's dir would draw it).
        std::optional<milo::Dir> geom(const milo::Dir &gh2, const std::vector<const milo::Dir *> &above,
                                      const milo::Dir &room, const milo::Dir &lighting,
                                      const std::set<std::string> &scripted, Drivers &drivers,
                                      std::set<std::string> &unreached)
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
            milo::Dir mats;
            for (size_t i = 0; i < gh2.entries.size(); ++i)
                if (gh2.entries[i].first == "Mat" &&
                    std::any_of(above.begin(), above.end(), [&](const milo::Dir *d) { return names(*d, gh2.entries[i].second); }))
                {
                    mats.entries.push_back(gh2.entries[i]);
                    mats.bodies.push_back(gh2.bodies[i]);
                }
            for (size_t i = 0; i < gh2.entries.size(); ++i)
                if (gh2.entries[i].first == "Tex" && names(mats, gh2.entries[i].second))
                    milo::add(out, "Tex", gh2.entries[i].second, gh2.bodies[i]);
            for (size_t i = 0; i < mats.entries.size(); ++i)
                milo::add(out, "Mat", mats.entries[i].second, std::move(mats.bodies[i]));
            // GH1's camera deletes the room's target_parent.mesh for its own
            // (VenueCam's constructor, GH1 0x16f18c).
            std::set<std::string> taken = {"target_parent.mesh"};
            for (const auto &e : out.entries)
                taken.insert(e.second);
            static const std::set<std::string> kClasses = {
                "Tex", "Mat", "Mesh", "View", "Light", "Environ", "TransAnim", "MatAnim",
                "LightAnim", "EnvAnim", "MeshAnim", "Flare", "ParticleSys", "ParticleSysAnim",
            };
            for (const auto &[scene, top] : {std::pair{&room, kRoom}, std::pair{&lighting, kLighting}})
                for (Object &o : objects(*scene, kClasses, top, taken, {}, scripted, drivers))
                    if (taken.insert(o.name).second)
                    {
                        if (o.unreached)
                            unreached.insert(o.name);
                        milo::add(out, o.cls, o.name, std::move(o.body));
                    }
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

        std::string numbered(const char *prefix, int nn, const char *suffix)
        {
            return std::string(prefix) + (nn < 10 ? "0" : "") + std::to_string(nn) + suffix;
        }

        // A Waypoint 3 as `from` is, on that spot, with those flags and
        // links to those (Waypoint::Load, 0x192068).
        Bytes waypoint(const Bytes &from, size_t trans, const Bytes &spot, uint32_t flags,
                       const std::vector<std::string> &links)
        {
            size_t o = transEnd(from, trans);
            Bytes out(from.begin(), from.begin() + static_cast<std::ptrdiff_t>(o));
            for (const size_t t : {trans + 4u, trans + 52u})
                std::copy(spot.begin(), spot.end(), out.begin() + static_cast<std::ptrdiff_t>(t));
            putU32(out, flags);
            putU32(out, static_cast<uint32_t>(links.size()));
            for (const std::string &link : links)
                putStr(out, link);
            const uint32_t had = u32(from, o + 4u);
            o += 8u;
            for (uint32_t i = 0; i < had; ++i)
                str(from, o);
            out.insert(out.end(), from.begin() + static_cast<std::ptrdiff_t>(o), from.end());
            return out;
        }

        // GH2's chars dir with GH1's waypoints for its own (flags in
        // macros.dta). The band's starts are on GH1's stage spots and the
        // guitarist's on walk spot 01; GH1 has no second guitarist, and the
        // two start on 02 and 01. Each walk spot has a waypoint, `onWalk`
        // its name: the last is where a walk turns and never where one ends
        // (StartWalk, GH1 0x284818), the rest link to it and are walked to
        // where the venue has `walks`, `solo` before a solo.
        std::optional<milo::Dir> chars(const milo::Dir &gh2, const Spots &at, bool walks, int solo,
                                       std::vector<std::string> &onWalk)
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
                kWalkSpot = 64u,
                kSoloWalkSpot = 128u,
                kStartGuitarist0Mp = 512u,
                kStarts = kStartGuitarist0 | kStartGuitarist1Mp | kStartSinger | kStartKeyboardist | kStartBassist |
                          kStartDrummer | kStartGuitarist0Mp,
            };
            milo::Dir out = gh2;
            out.entries.clear();
            out.bodies.clear();
            std::optional<std::pair<Bytes, size_t>> start; // the guitarist's, and its Trans
            for (size_t i = 0; i < gh2.entries.size(); ++i)
            {
                if (gh2.entries[i].first != "Waypoint")
                {
                    out.entries.push_back(gh2.entries[i]);
                    out.bodies.push_back(gh2.bodies[i]);
                    continue;
                }
                const Bytes &b = gh2.bodies[i];
                const auto trans = waypointTrans(b);
                if (!trans)
                    return std::nullopt;
                const uint32_t flags = u32(b, transEnd(b, *trans));
                const Bytes *spot = nullptr;
                if (flags & kStartGuitarist0)
                    spot = &at.walk[0];
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
                if (!spot)
                    continue;
                out.entries.push_back(gh2.entries[i]);
                out.bodies.push_back(waypoint(b, *trans, *spot, flags & kStarts, {}));
                if (flags & kStartGuitarist0)
                    start = {out.bodies.back(), *trans};
            }
            if (!start)
                return std::nullopt;
            const size_t spots = at.walk.size() > 1u ? at.walk.size() - 1u : 1u;
            onWalk.clear();
            for (size_t i = 0; i < at.walk.size(); ++i)
                onWalk.push_back(numbered("walk_spot_", static_cast<int>(i) + 1, ".way"));
            for (size_t i = 0; i < at.walk.size(); ++i)
            {
                const bool turn = i >= spots;
                const uint32_t flags =
                    turn || !walks ? 0u : kWalkSpot | (static_cast<int>(i) == solo ? kSoloWalkSpot : 0u);
                std::vector<std::string> links;
                if (turn)
                    links.assign(onWalk.begin(), onWalk.begin() + static_cast<std::ptrdiff_t>(spots));
                else if (spots < at.walk.size())
                    links.push_back(onWalk[spots]);
                milo::add(out, "Waypoint", onWalk[i], waypoint(start->first, start->second, at.walk[i], flags, links));
            }
            onWalk.resize(spots);
            return out;
        }

        // That object's body in the first of those scenes that has it.
        const Bytes *object(const std::vector<const milo::Dir *> &scenes, const char *cls, const std::string &name)
        {
            for (const milo::Dir *scene : scenes)
                for (size_t i = 0; scene && i < scene->entries.size(); ++i)
                    if (scene->entries[i].first == cls && scene->entries[i].second == name)
                        return &scene->bodies[i];
            return nullptr;
        }

        // Each Crowd<nn>.mm's places: a count, then that many transforms.
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
                places.emplace_back(found->begin() + static_cast<std::ptrdiff_t>(o), found->begin() + static_cast<std::ptrdiff_t>(end));
            }
            return places;
        }

        // The crowd members GH1 draws whole in each of a venue's regions,
        // each the Crowd<nn>.mm it is of and which of its places.
        //
        // A region is crowd_limits<nn>.mesh, a Mesh 25 (its Trans 8, Draw 1,
        // material, owner, nine bytes, then its verts, 48 bytes each, and
        // faces): the places that are over one of its faces and less than a
        // card's height above it, in the mesh's own space, as many as the
        // crowd has members to draw whole (Crowd::InitRegion, GH1 0x170da8;
        // PointInXY, GH1 0x1e3600). A shot names its region, and the flat
        // cards there give way to those members (Crowd::SwitchRegion, GH1
        // 0x1727b0). Its sphere is about the middle of the box those places
        // are in and as wide as the box is from corner to corner (GH1
        // 0x171378).
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
                if (u32(*mesh, 0u) != 25u || u32(*mesh, 4u) != 8u)
                    continue;
                // Its world transform, three rows and where it is, inverted.
                float m[12];
                for (size_t i = 0; i < 12u; ++i)
                    m[i] = f32(*mesh, 56u + i * 4u);
                const float det = m[0] * (m[4] * m[8] - m[5] * m[7]) - m[1] * (m[3] * m[8] - m[5] * m[6]) +
                                  m[2] * (m[3] * m[7] - m[4] * m[6]);
                if (det == 0.0f)
                    continue;
                const float inv[9] = {
                    (m[4] * m[8] - m[5] * m[7]) / det, (m[2] * m[7] - m[1] * m[8]) / det, (m[1] * m[5] - m[2] * m[4]) / det,
                    (m[5] * m[6] - m[3] * m[8]) / det, (m[0] * m[8] - m[2] * m[6]) / det, (m[2] * m[3] - m[0] * m[5]) / det,
                    (m[3] * m[7] - m[4] * m[6]) / det, (m[1] * m[6] - m[0] * m[7]) / det, (m[0] * m[4] - m[1] * m[3]) / det,
                };
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
                const auto over = [&](float x, float y)
                {
                    for (size_t f = 0; f < faceCount; ++f)
                    {
                        float px[3], py[3];
                        for (size_t c = 0; c < 3u; ++c)
                        {
                            const size_t v = (*mesh)[faces + f * 6u + c * 2u] | ((*mesh)[faces + f * 6u + c * 2u + 1u] << 8);
                            if (v >= count)
                                return false;
                            px[c] = f32(*mesh, verts + v * 48u);
                            py[c] = f32(*mesh, verts + v * 48u + 4u);
                        }
                        const auto side = [&](size_t a, size_t b) { return (x - px[a]) * (py[b] - py[a]) - (y - py[a]) * (px[b] - px[a]); };
                        const float s0 = side(0, 1), s1 = side(1, 2), s2 = side(2, 0);
                        if ((s0 >= 0.0f && s1 >= 0.0f && s2 >= 0.0f) || (s0 <= 0.0f && s1 <= 0.0f && s2 <= 0.0f))
                            return true;
                    }
                    return false;
                };
                Region &region = out.back();
                float low[3] = {0.0f, 0.0f, 0.0f}, high[3] = {0.0f, 0.0f, 0.0f};
                for (size_t c = 0; c < places.size(); ++c)
                    for (uint32_t k = 0; k < u32(places[c], 0u) && region.members.size() < whole; ++k)
                    {
                        const size_t at = 4u + static_cast<size_t>(k) * 48u + 36u;
                        const float p[3] = {f32(places[c], at), f32(places[c], at + 4u), f32(places[c], at + 8u)};
                        const float d[3] = {p[0] - m[9], p[1] - m[10], p[2] - m[11]};
                        const float x = d[0] * inv[0] + d[1] * inv[3] + d[2] * inv[6];
                        const float y = d[0] * inv[1] + d[1] * inv[4] + d[2] * inv[7];
                        const float z = d[0] * inv[2] + d[1] * inv[5] + d[2] * inv[8];
                        if (!(z > 0.0f && z < height && over(x, y)))
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

        // GH1's flat crowd in GH2's.
        //
        // GH1 keeps a card's place for each member of the crowd it draws
        // flat: Crowd<nn>.mm, a MultiMesh 0 (a Draw 1, its mesh, then the
        // places) for the nn-th of the crowd's archetypes (arena/crowd.dta;
        // Crowd::FinishLoading, GH1 0x171558). A card is crowd_flat_height
        // high, half that wide and about its place (FormFlatCrowd, GH1
        // 0x170160).
        //
        // GH2's WorldCrowd 6 (WorldCrowd::Load, 0x26c430) keeps the same: a
        // Draw 3, the mesh it was placed over, how many, a flag, each
        // member's Character with its card's height, density and radius,
        // then each one's places, and its Hmx::Object. Its card is as GH1's
        // (BuildBillboard, 0x26bba0). So `crowd`, which the shots show
        // (gh1/cameras.cpp), has GH1's places and height, with the stand-in's
        // crowd members for GH1's, male before female as GH1 lists them, and
        // any other WorldCrowd has none.
        //
        // Returns `crowd`'s stamp, the first word of what follows its places:
        // a shot's members are kept only if it has the same
        // (CamShot::Load, 0x265508).
        uint32_t crowd(milo::Dir &chars, const std::vector<Bytes> &places, float height)
        {
            uint32_t stamp = 0xffffffffu;
            std::vector<std::string> all;
            for (const auto &[cls, name] : chars.entries)
                if (cls == "Character" && name.rfind("crowd_", 0) == 0)
                    all.push_back(name);
            std::sort(all.begin(), all.end(), [](const std::string &l, const std::string &r)
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
                out.insert(out.end(), b.begin() + static_cast<std::ptrdiff_t>(o), b.end());
                chars.bodies[i] = std::move(out);
            }
            return stamp;
        }
    }

    void addVenues(size_t layer, size_t disc)
    {
        // GH1's venues, each with the View that is its drum kit: part of the
        // room, where GH2's is its drummer's.
        static const std::pair<const char *, const char *> kVenues[] = {
            {"basement", "drum_kit.view"}, {"small_club", "drumkit.view"}, {"big_club", "drum_kit.view"},
            {"theatre", "drum_kit.view"},  {"fest", "drum_kit.view"},      {"arena", "drum kit 00.view"},
        };
        dtb::Macros none;
        // How high a flat crowd member's card is (crowd_flat_height in
        // system/run/config/arena.dta, merged into config/gh.dta's arena).
        float flatHeight = 100.8f;
        if (const auto config = dtb::read("config/gh.dta", none, [disc](const std::string &path) { return ark::readFile(disc, path); }))
            if (const dtb::Node *arena = dtb::find(*config, "arena"))
                if (const dtb::Node *found = dtb::find(*arena, "crowd_flat_height"); found && found->nodes.size() > 1u)
                    flatHeight = dtb::number(found->nodes[1]).value_or(flatHeight);
        // Whether the guitarist walks in each venue, and the walk spot a
        // solo is played on (allow_walks and solo_walk_point, by its number
        // less one, in arena/venues.dta).
        const auto settings =
            dtb::read("arena/venues.dta", none, [disc](const std::string &path) { return ark::readFile(disc, path); });
        const auto setting = [&](const char *of, const char *key, float fallback)
        {
            const dtb::Node *theirs = settings ? dtb::find(*settings, of) : nullptr;
            const dtb::Node *found = theirs ? dtb::find(*theirs, key) : nullptr;
            return found && found->nodes.size() > 1u ? dtb::number(found->nodes[1]).value_or(fallback) : fallback;
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
            Drivers drivers;
            std::set<std::string> unreached;
            const auto madeGeom =
                geom(*gh2Geom, {&*gh2Chars, &*gh2Lights}, *room, *lighting, scripted(disc, name), drivers, unreached);
            const Spots at = spots({&*lighting, &*room});
            Stage stage;
            auto madeChars = chars(*gh2Chars, at, setting(name, "allow_walks", 0.0f) != 0.0f,
                                   static_cast<int>(setting(name, "solo_walk_point", -1.0f)), stage.walks);
            if (!madeGeom || !madeChars)
            {
                std::cerr << "[gh1] cannot build " << name << "'s venue" << std::endl;
                continue;
            }
            const auto crowdScene = load(disc, theirs + "crowd.rnd_ps2");
            const std::vector<const milo::Dir *> crowdScenes = {&*lighting, &*room, crowdScene ? &*crowdScene : nullptr};
            const std::vector<Bytes> places = crowdPlaces(crowdScenes);
            stage.crowdStamp = crowd(*madeChars, places, flatHeight);
            // The members GH1 draws whole: arena/crowd.dta's instances, ten,
            // two more outside the festival, and five more again in the
            // basement and the small club.
            const std::string venueName = name;
            const size_t whole = venueName == "fest" ? 10u : venueName == "basement" || venueName == "small_club" ? 17u : 12u;
            stage.regions = crowdRegions(crowdScenes, places, flatHeight, whole);
            ark::addFile(layer, geomPath, milo::write(*madeGeom));
            ark::addFile(layer, lightsPath, milo::write(lights(*gh2Lights)));
            ark::addFile(layer, charsPath, milo::write(*madeChars));
            stage.spot = at.stage[0];
            addCameras(layer, disc, name, ours, stage);
            std::set<std::string> present;
            for (const auto &e : madeGeom->entries)
                if (!unreached.count(e.second))
                    present.insert(e.second);
            addScripts(layer, disc, name, ours, drivers, present, kit, stage.walks);
            // GH1's crowd streams, which the type names (gh1/scripts.cpp).
            // What is not made here is the stand-in's: its sound bank and
            // encore streams.
            const std::string streams = std::string("venues/") + name + "/streams/";
            for (int version = 1; version < 10; ++version)
                for (const char *level : {"0intro", "1danger", "2poor", "3norm", "4good"})
                {
                    const std::string file = "crowd_v" + std::to_string(version) + "_" + level + ".vgs";
                    ark::lend(layer, world + "streams/" + file, disc, streams + file);
                }
        }
    }
}
