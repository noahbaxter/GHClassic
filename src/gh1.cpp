// GH1's guitarists in GH2, each as one more outfit of its character.
//
// GH1 keeps a guitarist as one scene (charsys/<x>/gen/<x>.rnd, a v10 milo)
// of Tex 8, Mat 21 and Mesh 25 objects, its bones Meshes named bone_*.mesh.
// GH2's outfit for the same character (a v24 BandCharacter) has Trans bones
// by those same names plus everything that makes a character run: drivers,
// IK, hair, eyes, lip sync. The outfit here is GH2's, its skin meshes swapped
// for GH1's objects as they are, which GH2's loaders still read (RndMesh::Load
// 0x3d5420 and RndMat::Load 0x1bfc00 keep paths for those revisions), and its
// LOD groups listing GH1's meshes from GH1's own lod views. GH2's anims drive
// the body; GH1's face has no bones for GH2's visemes, so it stays still.

#include "gh1.h"

#include "disc/ark.h"
#include "milo/milo.h"
#include "outfits.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <functional>
#include <iostream>
#include <map>
#include <set>

namespace gh2
{
    namespace
    {
        using milo::Bytes;

        struct Outfit
        {
            const char *folder;    // GH1's, under charsys/
            const char *character; // GH2's, in config's (characters ...)
            const char *base;      // GH2's outfit it is built on
            const char *name;      // ours
            const char *label;     // the outfit picker's (sel_character.dta)
            const char *highway;   // GH1's, track/surfaces/<highway>.bmp
        };

        // GH1's locale names the folders: hair_metal Izzy, nu_metal Pandora,
        // hiphop Xavier.
        constexpr Outfit kOutfits[] = {
            {"punk", "punk", "punk1", "punkgh1", "GH1 MOHAWK", "punk"},
            {"alterna", "alterna", "alterna1", "alternagh1", "GH1 SKULLS", "alterna"},
            {"metal", "metal", "metal1", "metalgh1", "GH1 SHIRT", "metal"},
            {"hair_metal", "glam", "glam1", "glamgh1", "GH1 CODPIECE", "hair"},
            {"nu_metal", "goth", "goth2", "gothgh1", "GH1 LEATHERS", "nu_metal"},
            {"hiphop", "funk1", "funk1", "funk1gh1", "GH1", "hiphop"},
            {"classic", "classic", "classic", "classicgh1", "GH1", "classic"},
            {"grim", "grim", "grim", "grimgh1", "GH1", "grim"},
        };

        // Characters whose one outfit the locale never named.
        constexpr const char *kUnnamed[] = {"funk1", "classic", "grim"};

        uint32_t u32(const Bytes &b, size_t o)
        {
            uint32_t v = 0u;
            if (o + 4u <= b.size())
                std::memcpy(&v, b.data() + o, 4u);
            return v;
        }

        // A length-prefixed string at o; o moves past it.
        std::string str(const Bytes &b, size_t &o)
        {
            const uint32_t n = u32(b, o);
            o += 4u;
            if (o + n > b.size())
            {
                o = b.size();
                return {};
            }
            std::string s(reinterpret_cast<const char *>(b.data() + o), n);
            o += n;
            return s;
        }

        void putU32(Bytes &out, uint32_t v)
        {
            const auto *p = reinterpret_cast<const uint8_t *>(&v);
            out.insert(out.end(), p, p + 4);
        }

        void putStr(Bytes &out, const std::string &s)
        {
            putU32(out, static_cast<uint32_t>(s.size()));
            out.insert(out.end(), s.begin(), s.end());
        }

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

        // GH1's Trans 8, inside its Mesh 25 at offset 4: rev, local, world,
        // then child names (GH1's hierarchy; its parent field names itself).
        struct Bone
        {
            Bytes world;
            std::vector<std::string> children;
        };

        Bone gh1Bone(const Bytes &body)
        {
            Bone bone{Bytes(body.begin() + 56, body.begin() + 104), {}};
            size_t o = 108u;
            for (uint32_t i = 0, n = u32(body, 104u); i < n; ++i)
                bone.children.push_back(str(body, o));
            return bone;
        }

        // A GH1 Mesh 25's bone slots: Trans 8, Draw 1, mat, owner, two u32s,
        // bsp, verts (48 bytes), faces (6), group sizes, then four bone names
        // unless the first word is 0.
        std::vector<std::string> meshBones(const Bytes &body)
        {
            size_t o = 108u; // mesh rev, trans rev, local, world, child count
            for (uint32_t i = 0, n = u32(body, 104u); i < n; ++i)
                str(body, o);
            o += 4u; // constraint
            str(body, o);
            o += 1u;
            str(body, o);
            o += 4u + 1u; // Draw rev, showing
            const uint32_t drawChildren = u32(body, o);
            o += 4u;
            for (uint32_t i = 0; i < drawChildren; ++i)
                str(body, o);
            o += 16u;
            str(body, o);
            str(body, o);
            o += 9u;
            o += 4u + 48u * u32(body, o);
            o += 4u + 6u * u32(body, o);
            o += 4u + u32(body, o);
            if (u32(body, o) == 0u)
                return {};
            std::vector<std::string> bones;
            for (int i = 0; i < 4; ++i)
            {
                std::string b = str(body, o);
                if (!b.empty())
                    bones.push_back(std::move(b));
            }
            return bones;
        }

        // A transform as stored: three axis rows, then the position. Points
        // are rows: world = local * parent, as GH2's own bones check out.
        struct Xfm
        {
            float m[9];
            float v[3];
        };

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

        // GH2's Trans 9: rev, object header, local, world, constraint,
        // target, preserve scale, parent.
        constexpr size_t kTransLocal = 13u, kTransWorld = 61u;

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

        bool startsWith(const std::string &s, const char *prefix)
        {
            return s.rfind(prefix, 0) == 0;
        }

        milo::Dir graft(const milo::Dir &gh2, const milo::Dir &gh1)
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
            std::map<std::string, Bone> bones;
            std::set<std::string> meshes;
            for (size_t i = 0; i < gh1.entries.size(); ++i)
            {
                const auto &[c, n] = gh1.entries[i];
                if (c == "Mesh" && startsWith(n, "bone_"))
                    bones.emplace(n, gh1Bone(gh1.bodies[i]));
                else if ((c == "Tex" || c == "Mat" || c == "Mesh") && !startsWith(n, "spot_"))
                {
                    added.push_back({c, n, gh1.bodies[i]});
                    if (c == "Mesh")
                        meshes.insert(n);
                }
            }

            // Bones the skin is weighted to that GH2's skeleton lacks (hair,
            // a cloak, an extra neck), with any missing ancestors, under their
            // GH1 parent. GH2's anims don't drive them; they ride the parent.
            std::map<std::string, std::string> owner;
            for (const auto &[n, bone] : bones)
                for (const std::string &child : bone.children)
                    owner[child] = n;
            std::vector<std::string> needed;
            for (const Object &o : added)
            {
                if (o.cls != "Mesh")
                    continue;
                std::vector<std::string> used = meshBones(o.body);
                if (owner.count(o.name))
                    used.push_back(owner[o.name]);
                for (std::string b : used)
                {
                    while (bones.count(b) && !gh2Names.count(b) &&
                           std::find(needed.begin(), needed.end(), b) == needed.end())
                    {
                        needed.push_back(b);
                        b = owner.count(b) ? owner[b] : std::string();
                    }
                }
            }
            // GH1's body on GH2's skeleton: each bone both have moves to GH1's
            // place, keeping GH2's axes, which GH2's anims turn. GH2's own
            // bones ride along; GH1's extra ones keep their GH1 world.
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
                        world = g->second.world;
                        std::memcpy(world.v, bones[n].world.data() + 36, 12u);
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

            // LODs from GH1's own views; lod0 everywhere when there's no lod1.
            // top.view's pieces draw at every LOD, but its eyes: GH2's stay,
            // as CharEyes moves them.
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
            for (const std::string &n : top)
            {
                if (n.find("eye") != std::string::npos)
                    continue;
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

    void installGh1()
    {
        const auto gh1Disc = ark::discWithSerial("SLUS_212.24");
        const auto gh2Disc = ark::discWithSerial("SLUS_214.47");
        if (!gh1Disc || !gh2Disc)
            return;
        const auto start = std::chrono::steady_clock::now();
        size_t built = 0u;
        for (const Outfit &outfit : kOutfits)
        {
            const std::string folder = outfit.folder, base = outfit.base, name = outfit.name;
            const auto gh1 = load("charsys/" + folder + "/gen/" + folder + ".rnd_ps2");
            if (!gh1)
            {
                std::cerr << "[gh1] cannot read " << folder << "'s model" << std::endl;
                continue;
            }
            bool ok = true;
            for (const char *suffix : {"", "_ui"})
            {
                const auto gh2 = load("char/" + base + "/og/gen/" + base + suffix + ".milo_ps2");
                if (!gh2)
                {
                    std::cerr << "[gh1] cannot read " << base << suffix << std::endl;
                    ok = false;
                    break;
                }
                ark::addFile("char/" + name + "/og/gen/" + name + suffix + ".milo_ps2",
                             milo::write(graft(*gh2, *gh1)));
            }
            if (!ok)
                continue;
            // _horse and anything else beside it is GH2's base outfit's, and
            // so are the photos. The highway (track/surfaces/%s_keep.bmp) is
            // GH1's, which GH2's loader reads as its own.
            ark::rename("char/" + name + "/og/gen/" + name, *gh2Disc, "char/" + base + "/og/gen/" + base);
            ark::rename("track/surfaces/gen/" + name + "_keep", *gh1Disc,
                        std::string("track/surfaces/gen/") + outfit.highway);
            outfits::photosFrom(name, *gh2Disc, base);
            outfits::add(outfit.character, name, base, outfit.label);
            ++built;
        }
        for (const char *character : kUnnamed)
            outfits::label(character, character, "CLASSIC");
        const auto ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
        std::cerr << "[gh1] " << built << " outfits built in " << ms << " ms" << std::endl;
    }
}
