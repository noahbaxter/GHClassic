// GH1's skeleton and skin on GH2's outfit; gh1/install.cpp has the whole.

#include "gh1/rig.h"

#include "disc/ark.h"

#include <algorithm>
#include <cstring>
#include <functional>
#include <iostream>
#include <set>

namespace gh2
{
    namespace
    {
        using gh1::Bone;
        using gh1::bytes;
        using gh1::Xfm;
        using milo::Bytes;
        using milo::putStr;
        using milo::putU32;
        using milo::str;
        using milo::u32;

        // Group rev 12: 163 bytes, a count, the names, then the rest.
        constexpr size_t kGroupNames = 163u;

        std::vector<std::string> groupNames(const Bytes &body, size_t *end = nullptr)
        {
            std::vector<std::string> names;
            size_t o = kGroupNames + 4u;
            for (uint32_t i = 0, n = u32(body, kGroupNames); i < n; ++i)
                names.push_back(str(body, o));
            if (end)
                *end = o;
            return names;
        }

        Bytes groupWith(const Bytes &body, const std::vector<std::string> &names)
        {
            size_t end = 0u;
            groupNames(body, &end);
            Bytes out(body.begin(), body.begin() + kGroupNames);
            putU32(out, static_cast<uint32_t>(names.size()));
            for (const std::string &n : names)
                putStr(out, n);
            out.insert(out.end(), body.begin() + static_cast<std::ptrdiff_t>(end), body.end());
            return out;
        }

        // The mesh names a GH1 View (rev 7) lists, by its strings.
        std::vector<std::string> viewMeshes(const Bytes &body, const std::set<std::string> &meshes)
        {
            std::vector<std::string> out;
            for (size_t o = 0; o + 4u <= body.size(); ++o)
            {
                const uint32_t n = u32(body, o);
                if (n == 0u || n > 64u || o + 4u + n > body.size())
                    continue;
                const std::string s(reinterpret_cast<const char *>(body.data() + o + 4u), n);
                if (meshes.count(s) && std::find(out.begin(), out.end(), s) == out.end())
                    out.push_back(s);
            }
            return out;
        }

        Bone gh1Bone(const Bytes &body)
        {
            Bone bone{Bytes(body.begin() + 56, body.begin() + 104), {}};
            size_t o = 108u;
            for (uint32_t i = 0, n = u32(body, 104u); i < n; ++i)
                bone.children.push_back(str(body, o));
            return bone;
        }

        Bytes trans9(const Xfm &local, const Xfm &world, const std::string &parent)
        {
            Bytes out;
            putU32(out, 9u);
            out.insert(out.end(), 9u, 0u);
            const Bytes l = bytes(local), w = bytes(world);
            out.insert(out.end(), l.begin(), l.end());
            out.insert(out.end(), w.begin(), w.end());
            putU32(out, 0u);
            putStr(out, "");
            out.push_back(0u);
            putStr(out, parent);
            return out;
        }

        // A GH1 Mesh 25 under `parent`: its Trans 8, whose parent field GH2
        // never reads (RndTransformable::Load, 0x3d72d0, takes rev 8's link
        // from the parent's child list), rewritten as Trans 9 naming it. One
        // with children of its own keeps them and stays unparented. `local`
        // keeps its GH1 world under the bone as posed here.
        Bytes reparent(const Bytes &body, const std::string &parent, const Xfm &local)
        {
            if (u32(body, 104u) != 0u)
                return body;
            const size_t constraint = 108u;
            size_t o = constraint + 4u;
            str(body, o);
            o += 1u;
            const size_t ownParent = o;
            str(body, o);
            Bytes out(body.begin(), body.begin() + 4);
            putU32(out, 9u);
            const Bytes l = bytes(local);
            out.insert(out.end(), l.begin(), l.end());
            out.insert(out.end(), body.begin() + 56, body.begin() + 104);
            out.insert(out.end(), body.begin() + static_cast<std::ptrdiff_t>(constraint),
                       body.begin() + static_cast<std::ptrdiff_t>(ownParent));
            putStr(out, parent);
            out.insert(out.end(), body.begin() + static_cast<std::ptrdiff_t>(o), body.end());
            return out;
        }

        // The first mesh an object's body names (bone_head.mesh).
        std::string namedMesh(const Bytes &body)
        {
            for (size_t o = 0; o + 4u <= body.size(); ++o)
            {
                const uint32_t n = u32(body, o);
                if (n < 6u || n > 64u || o + 4u + n > body.size())
                    continue;
                const std::string s(reinterpret_cast<const char *>(body.data() + o + 4u), n);
                if (s.compare(n - 5u, 5u, ".mesh") == 0)
                    return s;
            }
            return {};
        }

        // A GH2 Mesh 28 left out of drawing: rev, object header, Trans 9
        // (rev, local, world, constraint, target, preserve, parent), then
        // Draw's rev and its showing flag.
        void hide(Bytes &mesh)
        {
            size_t o = 4u + 9u + 4u + 96u + 4u;
            str(mesh, o);
            o += 1u;
            str(mesh, o);
            o += 4u;
            if (o < mesh.size())
                mesh[o] = 0u;
        }

        // GH1's Morph 3 as GH2's RndMorph::Load (0x201800) reads it, which
        // still takes rev 3 and its poses (a mesh, then (weight, frame) keys).
        // Its Animatable goes from GH1's rev 0 (empty filter and child lists)
        // to GH2's 4 (rate, units). The target and normals are what GH1's
        // CharFace::PostLoad (GH1 0x2a6f10) set at load: `target`, normals on.
        std::optional<Bytes> gh2Morph(const Bytes &gh1, const std::string &target)
        {
            if (u32(gh1, 0u) != 3u || u32(gh1, 4u) != 0u || u32(gh1, 8u) != 0u || u32(gh1, 12u) != 0u)
                return std::nullopt;
            size_t o = 16u;
            const uint32_t poses = u32(gh1, o);
            o += 4u;
            for (uint32_t i = 0; i < poses && o < gh1.size(); ++i)
            {
                str(gh1, o);
                o += 4u + 8u * u32(gh1, o);
            }
            const size_t posesEnd = o;
            str(gh1, o);
            if (o + 6u != gh1.size())
                return std::nullopt;
            Bytes out;
            for (const uint32_t v : {3u, 4u, 0u, 0u})
                putU32(out, v);
            out.insert(out.end(), gh1.begin() + 16, gh1.begin() + static_cast<std::ptrdiff_t>(posesEnd));
            putStr(out, target);
            out.push_back(1u);
            out.insert(out.end(), gh1.begin() + static_cast<std::ptrdiff_t>(o + 1u), gh1.end());
            return out;
        }

        bool startsWith(const std::string &s, const char *prefix)
        {
            return s.rfind(prefix, 0) == 0;
        }
    }

    namespace gh1
    {
        std::map<std::string, Bone> gh1Bones(const milo::Dir &gh1)
        {
            std::map<std::string, Bone> bones;
            for (size_t i = 0; i < gh1.entries.size(); ++i)
                if (gh1.entries[i].first == "Mesh" && gh1.entries[i].second.rfind("bone_", 0) == 0)
                    bones.emplace(gh1.entries[i].second, gh1Bone(gh1.bodies[i]));
            return bones;
        }

        std::map<std::string, std::string> owners(const std::map<std::string, Bone> &bones)
        {
            std::map<std::string, std::string> owner;
            for (const auto &[n, bone] : bones)
                for (const std::string &child : bone.children)
                    owner[child] = n;
            return owner;
        }

        Xfm xfm(const Bytes &b, size_t o)
        {
            Xfm x;
            std::memcpy(x.m, b.data() + o, 36u);
            std::memcpy(x.v, b.data() + o + 36u, 12u);
            return x;
        }

        Bytes bytes(const Xfm &x)
        {
            Bytes out(48u);
            std::memcpy(out.data(), x.m, 36u);
            std::memcpy(out.data() + 36u, x.v, 12u);
            return out;
        }

        Xfm operator*(const Xfm &a, const Xfm &b)
        {
            Xfm r{};
            for (int i = 0; i < 3; ++i)
                for (int j = 0; j < 3; ++j)
                    for (int k = 0; k < 3; ++k)
                        r.m[i * 3 + j] += a.m[i * 3 + k] * b.m[k * 3 + j];
            for (int j = 0; j < 3; ++j)
            {
                r.v[j] = b.v[j];
                for (int k = 0; k < 3; ++k)
                    r.v[j] += a.v[k] * b.m[k * 3 + j];
            }
            return r;
        }

        Xfm inverse(const Xfm &a)
        {
            const float *m = a.m;
            const float det = m[0] * (m[4] * m[8] - m[5] * m[7]) - m[1] * (m[3] * m[8] - m[5] * m[6]) +
                              m[2] * (m[3] * m[7] - m[4] * m[6]);
            const float d = det != 0.0f ? 1.0f / det : 0.0f;
            Xfm r{{(m[4] * m[8] - m[5] * m[7]) * d, (m[2] * m[7] - m[1] * m[8]) * d, (m[1] * m[5] - m[2] * m[4]) * d,
                   (m[5] * m[6] - m[3] * m[8]) * d, (m[0] * m[8] - m[2] * m[6]) * d, (m[2] * m[3] - m[0] * m[5]) * d,
                   (m[3] * m[7] - m[4] * m[6]) * d, (m[1] * m[6] - m[0] * m[7]) * d, (m[0] * m[4] - m[1] * m[3]) * d},
                  {}};
            for (int j = 0; j < 3; ++j)
                for (int k = 0; k < 3; ++k)
                    r.v[j] -= a.v[k] * r.m[k * 3 + j];
            return r;
        }

        milo::Dir graft(const milo::Dir &gh2, const milo::Dir &gh1, const milo::Dir &face, const std::string &outfit)
        {
            std::set<std::string> shadow;
            std::set<std::string> gh2Names;
            for (size_t i = 0; i < gh2.entries.size(); ++i)
            {
                gh2Names.insert(gh2.entries[i].second);
                if (gh2.entries[i].first == "Group" && gh2.entries[i].second.find("shadow") != std::string::npos)
                    for (const std::string &n : groupNames(gh2.bodies[i]))
                        shadow.insert(n);
            }

            // GH1's skin: its Tex, Mat and Mesh objects but the bones.
            struct Object
            {
                std::string cls, name;
                Bytes body;
            };
            std::vector<Object> added;
            std::map<std::string, Bone> bones = gh1Bones(gh1);
            std::set<std::string> meshes;
            for (size_t i = 0; i < gh1.entries.size(); ++i)
            {
                const auto &[c, n] = gh1.entries[i];
                if ((c == "Tex" || c == "Mat" || c == "Mesh") && !startsWith(n, "spot_") && !startsWith(n, "bone_"))
                {
                    added.push_back({c, n, gh1.bodies[i]});
                    if (c == "Mesh")
                        meshes.insert(n);
                }
            }
            // The face's poses (bad01.mesh) and the morphs blending them into
            // face.mesh and lashes.mesh. The poses draw in no group.
            for (size_t i = 0; i < face.entries.size(); ++i)
            {
                const auto &[c, n] = face.entries[i];
                if (c == "Mesh")
                    added.push_back({c, n, face.bodies[i]});
                else if (c == "Morph")
                {
                    auto morph = gh2Morph(face.bodies[i], n.substr(0, n.rfind('.')) + ".mesh");
                    if (morph)
                        added.push_back({c, outfit + "_" + n, std::move(*morph)});
                    else
                        std::cerr << "[gh1] cannot read morph " << n << std::endl;
                }
            }

            // Bones the skin is weighted to that GH2's skeleton lacks (hair,
            // a cloak, an extra neck), with any missing ancestors, under their
            // GH1 parent. GH2's anims don't drive them; they ride the parent.
            std::map<std::string, std::string> owner = owners(bones);
            std::vector<std::string> needed;
            for (const auto &bone : bones)
            {
                std::string b = bone.first;
                while (bones.count(b) && !gh2Names.count(b) &&
                       std::find(needed.begin(), needed.end(), b) == needed.end())
                {
                    needed.push_back(b);
                    b = owner.count(b) ? owner[b] : std::string();
                }
            }
            // GH1's rig on GH2's skeleton: each bone both have takes GH1's
            // rest, which GH1's skin is bound to and GH1's clips animate.
            // GH2's own bones ride along; GH1's extra ones join it.
            struct Gh2Bone
            {
                size_t index;
                Xfm local, world;
                std::string parent;
            };
            std::map<std::string, Gh2Bone> gh2Bones;
            for (size_t i = 0; i < gh2.entries.size(); ++i)
            {
                const Bytes &b = gh2.bodies[i];
                if (gh2.entries[i].first != "Trans" || u32(b, 0u) != 9u)
                    continue;
                size_t o = kTransWorld + 48u + 4u; // constraint
                str(b, o);
                o += 1u;
                gh2Bones[gh2.entries[i].second] = {i, xfm(b, kTransLocal), xfm(b, kTransWorld), str(b, o)};
            }
            std::map<std::string, std::pair<Xfm, Xfm>> posed; // local, world
            std::function<Xfm(const std::string &)> worldOf = [&](const std::string &n) -> Xfm
            {
                if (const auto it = posed.find(n); it != posed.end())
                    return it->second.second;
                Xfm local, world;
                if (const auto g = gh2Bones.find(n); g != gh2Bones.end())
                {
                    // A parent outside the bones stays where the stored pair
                    // puts it.
                    const Xfm parent = gh2Bones.count(g->second.parent) ? worldOf(g->second.parent)
                                                                        : inverse(g->second.local) * g->second.world;
                    if (bones.count(n))
                    {
                        world = xfm(bones[n].world, 0u);
                        local = world * inverse(parent);
                    }
                    else
                    {
                        local = g->second.local;
                        world = local * parent;
                    }
                }
                else
                {
                    world = xfm(bones[n].world, 0u);
                    local = owner.count(n) ? world * inverse(worldOf(owner[n])) : world;
                }
                posed[n] = {local, world};
                return world;
            };
            for (const auto &g : gh2Bones)
                worldOf(g.first);
            for (const std::string &b : needed)
            {
                worldOf(b);
                added.push_back({"Trans", b, trans9(posed[b].first, posed[b].second,
                                                    owner.count(b) ? owner[b] : std::string())});
            }

            // Rigid pieces hang off bones by the bones' child lists: hair,
            // earrings, belts, the face.
            for (Object &o : added)
                if (o.cls == "Mesh" && owner.count(o.name))
                    o.body = reparent(o.body, owner[o.name], xfm(o.body, 56u) * inverse(worldOf(owner[o.name])));

            // GH2's eyes are its own, sized for its heads and turned by its
            // CharLookAts along their axes, which GH1's don't share. GH2's
            // stay, unshown, for those to name (eye-L.mesh, goth2_EyeL.mesh;
            // classic has none); GH1's draw fixed to the head, as in GH1,
            // under names nothing of GH2's looks for.
            std::set<std::string> gh2Eyes{"eye-L.mesh", "eye-R.mesh"};
            for (size_t i = 0; i < gh2.entries.size(); ++i)
                if (gh2.entries[i].first == "CharLookAt")
                    gh2Eyes.insert(namedMesh(gh2.bodies[i]));
            for (Object &o : added)
                if (o.name == "L-eye.mesh" || o.name == "R-eye.mesh")
                    o.name = "gh1_" + o.name;

            // LODs from GH1's own views; lod0 everywhere when there's no lod1.
            // top.view's pieces draw at every LOD.
            std::vector<std::string> lod0, lod1, top;
            for (size_t i = 0; i < gh1.entries.size(); ++i)
            {
                const auto &[c, n] = gh1.entries[i];
                if (c == "View" && startsWith(n, "lod0"))
                    lod0 = viewMeshes(gh1.bodies[i], meshes);
                else if (c == "View" && startsWith(n, "lod1"))
                    lod1 = viewMeshes(gh1.bodies[i], meshes);
                else if (c == "View" && startsWith(n, "top"))
                    top = viewMeshes(gh1.bodies[i], meshes);
            }
            if (lod1.empty())
                lod1 = lod0;
            for (std::string n : top)
            {
                if (n == "L-eye.mesh" || n == "R-eye.mesh")
                    n = "gh1_" + n;
                for (auto *lod : {&lod0, &lod1})
                    if (std::find(lod->begin(), lod->end(), n) == lod->end())
                        lod->push_back(n);
            }

            // GH2's skin goes but the eyes (CharEyes and the lip servo name
            // them) and the shadow; GH1's objects win any name both have.
            std::set<std::string> addedNames;
            for (const Object &o : added)
                addedNames.insert(o.name);
            milo::Dir out = gh2;
            out.entries.clear();
            out.bodies.clear();
            for (size_t i = 0; i < gh2.entries.size(); ++i)
            {
                const auto &[c, n] = gh2.entries[i];
                std::string lower = n;
                std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
                const bool skin = c == "Mesh" && !shadow.count(n) && lower.find("eye") == std::string::npos;
                if (skin || addedNames.count(n))
                    continue;
                out.entries.push_back(gh2.entries[i]);
                out.bodies.push_back(c == "Group" && n.find("shadow") == std::string::npos
                                         ? groupWith(gh2.bodies[i], n.find("lod1") != std::string::npos ? lod1 : lod0)
                                         : gh2.bodies[i]);
                if (c == "Mesh" && gh2Eyes.count(n))
                    hide(out.bodies.back());
                if (const auto g = gh2Bones.find(n); g != gh2Bones.end() && g->second.index == i)
                {
                    const Bytes l = bytes(posed[n].first), w = bytes(posed[n].second);
                    std::copy(l.begin(), l.end(), out.bodies.back().begin() + kTransLocal);
                    std::copy(w.begin(), w.end(), out.bodies.back().begin() + kTransWorld);
                }
            }
            for (Object &o : added)
            {
                out.tableCount += 2u;
                out.tableSize += static_cast<uint32_t>(o.cls.size() + o.name.size() + 2u);
                out.entries.emplace_back(o.cls, o.name);
                out.bodies.push_back(std::move(o.body));
            }
            return out;
        }

        std::optional<milo::Dir> load(const std::string &path)
        {
            const auto file = ark::readFile(path);
            if (!file)
                return std::nullopt;
            const auto raw = milo::inflate(*file);
            if (!raw)
                return std::nullopt;
            return milo::parse(*raw);
        }
    }
}
