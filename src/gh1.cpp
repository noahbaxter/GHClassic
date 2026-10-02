// GH1's guitarists in GH2, each as one more outfit of its character.
//
// GH1 keeps a guitarist as one scene (charsys/<x>/gen/<x>.rnd, a v10 milo)
// of Tex 8, Mat 21 and Mesh 25 objects, its bones Meshes named bone_*.mesh.
// GH2's outfit for the same character (a v24 BandCharacter) has Trans bones
// by those same names plus everything that makes a character run: drivers,
// IK, hair, eyes, lip sync. The outfit here is GH2's, its skin meshes swapped
// for GH1's objects as they are, which GH2's loaders still read (RndMesh::Load
// 0x3d5420 and RndMat::Load 0x1bfc00 keep paths for those revisions), its
// LOD groups listing GH1's meshes from GH1's own lod views, and its bones at
// GH1's rest. The picker plays GH1's own idle, converted from GH1's clips;
// songs still play GH2's base clips, and GH1's face stays still.

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

        // A GH1 guitarist, as one more outfit of a GH2 character. GH1 names
        // most of its files after the folder; the rest are spelled out.
        struct Guitarist
        {
            const char *gh1Folder;           // charsys/<gh1Folder>/
            const char *gh2Character;        // in config's (characters ...)
            const char *baseOutfit;          // GH2's, the GH1 model is grafted onto
            const char *label;               // the outfit picker's (sel_character.dta)
            const char *clipPrefix = nullptr; // GH1's clip names, <clipPrefix>_idle_ui
            const char *highway = nullptr;   // GH1's, track/surfaces/<highway>.bmp
            bool pickerDoorOpens = true;     // the picker door swings open behind them

            std::string outfit() const { return std::string(gh2Character) + "gh1"; }
            std::string clips() const { return clipPrefix ? clipPrefix : gh1Folder; }
            std::string track() const { return highway ? highway : gh1Folder; }        };

        // GH1's locale names the folders: hair_metal Izzy, nu_metal Pandora,
        // hiphop Xavier.
        constexpr Guitarist kGuitarists[] = {
            // GH1's punk idles leaning on the back wall, where the door opens.
            {.gh1Folder = "punk", .gh2Character = "punk", .baseOutfit = "punk1", .label = "GH1 MOHAWK",
             .pickerDoorOpens = false},
            {.gh1Folder = "alterna", .gh2Character = "alterna", .baseOutfit = "alterna1", .label = "GH1 SKULLS"},
            {.gh1Folder = "metal", .gh2Character = "metal", .baseOutfit = "metal1", .label = "GH1 SHIRT"},
            {.gh1Folder = "hair_metal", .gh2Character = "glam", .baseOutfit = "glam1", .label = "GH1 CODPIECE",
             .clipPrefix = "hair", .highway = "hair"},
            {.gh1Folder = "nu_metal", .gh2Character = "goth", .baseOutfit = "goth2", .label = "GH1 LEATHERS",
             .clipPrefix = "nu"},
            {.gh1Folder = "hiphop", .gh2Character = "funk1", .baseOutfit = "funk1", .label = "GH1"},
            {.gh1Folder = "classic", .gh2Character = "classic", .baseOutfit = "classic", .label = "GH1"},
            {.gh1Folder = "grim", .gh2Character = "grim", .baseOutfit = "grim", .label = "GH1"},
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

        // GH1's bones (bone_*.mesh) by name.
        std::map<std::string, Bone> gh1Bones(const milo::Dir &gh1)
        {
            std::map<std::string, Bone> bones;
            for (size_t i = 0; i < gh1.entries.size(); ++i)
                if (gh1.entries[i].first == "Mesh" && gh1.entries[i].second.rfind("bone_", 0) == 0)
                    bones.emplace(gh1.entries[i].second, gh1Bone(gh1.bodies[i]));
            return bones;
        }

        // Each child's parent, by GH1's child lists.
        std::map<std::string, std::string> owners(const std::map<std::string, Bone> &bones)
        {
            std::map<std::string, std::string> owner;
            for (const auto &[n, bone] : bones)
                for (const std::string &child : bone.children)
                    owner[child] = n;
            return owner;
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

        // A sample set as CharBonesSamples keeps one: channels by name
        // (bone_x.quat) in type order, and per sample their values in that
        // order, positions 12 bytes, quats 16 and rotations 4, halved but
        // positions when compressed (LoadData 0x194978).
        struct Samples
        {
            std::vector<std::string> channels;
            uint32_t count = 0u;
            uint32_t compressed = 0u;
            Bytes data;

            size_t width(const std::string &channel) const
            {
                const std::string kind = channel.substr(channel.rfind('.') + 1);
                if (kind == "pos" || kind == "scale")
                    return 12u;
                return (kind == "quat" ? 16u : 4u) / (compressed ? 2u : 1u);
            }

            size_t stride() const
            {
                size_t s = 0u;
                for (const std::string &c : channels)
                    s += width(c);
                return s;
            }

            // A channel's bytes in a sample, or none.
            std::optional<Bytes> value(const std::string &channel, uint32_t sample) const
            {
                size_t o = stride() * sample;
                for (const std::string &c : channels)
                {
                    if (c == channel)
                        return Bytes(data.begin() + static_cast<std::ptrdiff_t>(o),
                                     data.begin() + static_cast<std::ptrdiff_t>(o + width(c)));
                    o += width(c);
                }
                return std::nullopt;
            }

            // A rotation channel after the rest, as rotations come last.
            void appendRotation(const std::string &channel, const std::function<Bytes(uint32_t)> &valueAt)
            {
                const size_t s = stride();
                Bytes out;
                for (uint32_t i = 0; i < count; ++i)
                {
                    out.insert(out.end(), data.begin() + static_cast<std::ptrdiff_t>(s * i),
                               data.begin() + static_cast<std::ptrdiff_t>(s * (i + 1)));
                    const Bytes v = valueAt(i);
                    out.insert(out.end(), v.begin(), v.end());
                }
                channels.push_back(channel);
                data = std::move(out);
            }
        };

        // Sample sets' headers, then their data. LoadHeader (0x194630) at
        // GH1's rev (5) takes a count, names, samples and compression; at
        // GH2's (10) a count, names, ten type offsets, compression and
        // samples.
        std::optional<std::vector<Samples>> readSamples(const Bytes &b, size_t o, uint32_t rev, int sets)
        {
            std::vector<Samples> out(static_cast<size_t>(sets));
            for (Samples &s : out)
            {
                const uint32_t n = u32(b, o);
                o += 4u;
                for (uint32_t i = 0; i < n && o < b.size(); ++i)
                    s.channels.push_back(str(b, o));
                if (rev >= 10u)
                {
                    o += 40u;
                    s.compressed = u32(b, o);
                    s.count = u32(b, o + 4u);
                }
                else
                {
                    s.count = u32(b, o);
                    s.compressed = u32(b, o + 4u);
                }
                o += 8u;
            }
            for (Samples &s : out)
            {
                const size_t size = s.stride() * s.count;
                if (o + size > b.size())
                    return std::nullopt;
                s.data.assign(b.begin() + static_cast<std::ptrdiff_t>(o),
                              b.begin() + static_cast<std::ptrdiff_t>(o + size));
                o += size;
            }
            return out;
        }

        // GH1's AnimClipSamples (Load, GH1 0x17fa40) after its class and
        // name: AnimClip rev 17+ (rev, start, end, rate, flags and two words
        // GH2 has its own of), then the samples' rev and two sets.
        struct Gh1Clip
        {
            float start = 0.0f, end = 0.0f, rate = 0.0f;
            uint32_t rev = 0u;
            std::vector<Samples> sets;
        };

        std::optional<Gh1Clip> readGh1Clip(const Bytes &acp)
        {
            size_t o = 0u;
            str(acp, o);
            str(acp, o);
            if (u32(acp, o) < 17u || o + 32u > acp.size())
                return std::nullopt;
            Gh1Clip clip;
            std::memcpy(&clip.start, acp.data() + o + 4u, 4u);
            std::memcpy(&clip.end, acp.data() + o + 8u, 4u);
            std::memcpy(&clip.rate, acp.data() + o + 12u, 4u);
            clip.rev = u32(acp, o + 28u);
            auto sets = readSamples(acp, o + 32u, clip.rev, 2);
            if (!sets)
                return std::nullopt;
            clip.sets = std::move(*sets);
            return clip;
        }

        // GH2's CharClipSamples (Load, 0x16b608): samples rev, then CharClip
        // (0x197000) rev 5: object header, start, end, rate, flags, play
        // flags, a float, a word, a bool, transitions (a clip, then its beat
        // pairs), events (enter, exit, then timed ones); then three sets.
        struct Gh2Clip
        {
            float start = 0.0f, end = 0.0f;
            size_t events = 0u, eventsEnd = 0u;
            std::vector<Samples> sets;
        };
        constexpr size_t kClipTiming = 17u, kClipFlags = 29u, kClipTransitions = 46u;

        std::optional<Gh2Clip> readGh2Clip(const Bytes &b)
        {
            if (u32(b, 0u) < 8u || u32(b, 4u) != 5u)
                return std::nullopt;
            Gh2Clip clip;
            std::memcpy(&clip.start, b.data() + kClipTiming, 4u);
            std::memcpy(&clip.end, b.data() + kClipTiming + 4u, 4u);
            size_t p = kClipTransitions;
            const uint32_t clips = u32(b, p);
            p += 4u;
            for (uint32_t i = 0; i < clips; ++i)
            {
                str(b, p);
                p += 4u + 8u * u32(b, p);
            }
            clip.events = p;
            str(b, p);
            str(b, p);
            const uint32_t timed = u32(b, p);
            p += 4u;
            for (uint32_t i = 0; i < timed; ++i)
            {
                p += 4u;
                str(b, p);
            }
            clip.eventsEnd = p;
            auto sets = readSamples(b, p, u32(b, 0u), 3);
            if (!sets)
                return std::nullopt;
            clip.sets = std::move(*sets);
            return clip;
        }

        // A GH1 clip where GH2's was: GH1's timing and samples at GH1's
        // rev, which GH2's CharBonesSamples still reads as GH1 wrote them,
        // `transitions` (a count, then per clip its name and (from, to) beat
        // pairs), and GH2's flags and events.
        Bytes writeClip(const Gh1Clip &gh1, const Bytes &gh2, const Gh2Clip &parts, const Bytes &transitions)
        {
            Bytes out;
            putU32(out, gh1.rev);
            out.insert(out.end(), gh2.begin() + 4, gh2.begin() + kClipTiming);
            for (const float f : {gh1.start, gh1.end, gh1.rate})
            {
                uint32_t v;
                std::memcpy(&v, &f, 4u);
                putU32(out, v);
            }
            out.insert(out.end(), gh2.begin() + kClipFlags, gh2.begin() + kClipTransitions);
            out.insert(out.end(), transitions.begin(), transitions.end());
            out.insert(out.end(), gh2.begin() + static_cast<std::ptrdiff_t>(parts.events),
                       gh2.begin() + static_cast<std::ptrdiff_t>(parts.eventsEnd));
            for (const Samples &s : gh1.sets)
            {
                putU32(out, static_cast<uint32_t>(s.channels.size()));
                for (const std::string &c : s.channels)
                    putStr(out, c);
                putU32(out, s.count);
                putU32(out, s.compressed);
            }
            for (const Samples &s : gh1.sets)
                out.insert(out.end(), s.data.begin(), s.data.end());
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

        // A GH1 clip's channels (bone_x.quat): two sample sets after
        // AnimClip, each a count, names, samples and compression.
        std::set<std::string> clipChannels(const Bytes &acp)
        {
            size_t o = 0u;
            str(acp, o);
            str(acp, o);
            o += 28u + 4u;
            std::set<std::string> out;
            for (int set = 0; set < 2; ++set)
            {
                const uint32_t n = u32(acp, o);
                o += 4u;
                for (uint32_t i = 0; i < n && o < acp.size(); ++i)
                    out.insert(str(acp, o));
                o += 8u;
            }
            return out;
        }

        // A clip set animates only the channels its CharBones declare (bone_x
        // with .trans or .mesh, as the set has it): CharBones::ScaleAdd
        // (0x168320) seeks each clip channel among them without end. One GH1
        // animates that GH2's lacks goes in as its parent's does, at GH1's
        // rest under that parent; each then declares GH1's channel types.
        bool declareBones(milo::Dir &set, const Bytes &acp, const milo::Dir &gh1)
        {
            const auto bones = gh1Bones(gh1);
            const auto owner = owners(bones);
            auto index = [&](const std::string &bone) -> std::optional<size_t>
            {
                for (size_t i = 0; i < set.entries.size(); ++i)
                {
                    const std::string &n = set.entries[i].second;
                    if (set.entries[i].first == "CharBone" && n.substr(0, n.rfind('.')) == bone)
                        return i;
                }
                return std::nullopt;
            };
            const std::set<std::string> channels = clipChannels(acp);
            std::vector<std::string> missing;
            for (const std::string &channel : channels)
            {
                const std::string b = channel.substr(0, channel.rfind('.'));
                if (!index(b) && std::find(missing.begin(), missing.end(), b) == missing.end())
                    missing.push_back(b);
            }
            // Parents first: each pass adds those whose parent is declared.
            for (bool added = true; added && !missing.empty();)
            {
                added = false;
                for (auto it = missing.begin(); it != missing.end();)
                {
                    const std::string mesh = *it + ".mesh";
                    const auto parent = owner.find(mesh);
                    if (!bones.count(mesh) || parent == owner.end())
                        return false;
                    const std::string up = parent->second.substr(0, parent->second.rfind('.'));
                    const auto from = index(up);
                    if (!from)
                    {
                        ++it;
                        continue;
                    }
                    const std::string &upName = set.entries[*from].second;
                    const std::string suffix = upName.substr(upName.rfind('.'));
                    // CharBone: rev, header, then Trans 9 (rev, local, world,
                    // constraint, target, preserve, parent) and its own fields.
                    const Bytes &t = set.bodies[*from];
                    size_t o = kTransWorld + 4u + 48u + 4u;
                    str(t, o);
                    o += 1u;
                    const size_t parentAt = o;
                    str(t, o);
                    const Xfm world = xfm(bones.at(mesh).world, 0u);
                    const Bytes local = bytes(world * inverse(xfm(bones.at(parent->second).world, 0u))),
                                worldBytes = bytes(world);
                    Bytes body(t.begin(), t.begin() + kTransLocal + 4);
                    body.insert(body.end(), local.begin(), local.end());
                    body.insert(body.end(), worldBytes.begin(), worldBytes.end());
                    body.insert(body.end(), t.begin() + kTransWorld + 4 + 48, t.begin() + static_cast<std::ptrdiff_t>(parentAt));
                    putStr(body, upName);
                    body.insert(body.end(), t.begin() + static_cast<std::ptrdiff_t>(o), t.end());
                    set.entries.emplace_back("CharBone", *it + suffix);
                    set.bodies.push_back(std::move(body));
                    set.tableCount += 2u;
                    set.tableSize += static_cast<uint32_t>(std::string("CharBone").size() + it->size() + suffix.size() + 2u);
                    it = missing.erase(it);
                    added = true;
                }
            }
            if (!missing.empty())
                return false;
            // CharBone::Load (0x1673a8) ends on a position flag, a flag, the
            // rotation's type (CharBones::TypeOf: quat 2 to rotz 5, 9 none)
            // and a word.
            constexpr const char *kRotations[] = {"quat", "rotx", "roty", "rotz"};
            for (const std::string &channel : channels)
            {
                const auto dot = channel.rfind('.');
                Bytes &body = set.bodies[*index(channel.substr(0, dot))];
                const std::string kind = channel.substr(dot + 1);
                if (kind == "pos")
                    body[body.size() - 10u] = 1u;
                for (uint32_t t = 0; t < 4u; ++t)
                    if (kind == kRotations[t])
                    {
                        const uint32_t type = 2u + t;
                        std::memcpy(body.data() + body.size() - 8u, &type, 4u);
                    }
            }
            return true;
        }

        // GH2's picker door swings on its character's clips (bone_door.rotz,
        // opening through ui_enter, open in ui_loop). GH1's characters stand
        // where it swings, so theirs is open from the start: the door's last
        // place in the clip replaced, held in GH1's one-sample set.
        void openDoor(Gh1Clip &clip, const Gh2Clip &gh2)
        {
            const std::string door = "bone_door.rotz";
            for (const Samples &from : gh2.sets)
            {
                if (from.count == 0u || !from.value(door, 0u))
                    continue;
                const Bytes open = *from.value(door, from.count - 1u);
                Samples &to = clip.sets[1];
                if (to.count == 1u && to.compressed == from.compressed)
                    to.appendRotation(door, [&](uint32_t) { return open; });
                return;
            }
        }

        // The clip set the picker plays, as GH2's base has it (its bones and
        // filter, its enter clip opening the door) with GH1's own picker
        // idle in every clip, leading into ui_loop where GH1's graph loops.
        std::optional<Bytes> pickerSet(const Guitarist &guitarist, const std::string &gh2Set, const milo::Dir &gh1)
        {
            const std::string editor = std::string("charsys/") + guitarist.gh1Folder + "/anims/editor/";
            auto set = load("char/" + gh2Set + "/anims/gen/" + gh2Set + "_ui.milo_ps2");
            const auto acp = ark::readFile(editor + "gen/" + guitarist.clips() + "_idle_ui.acp");
            auto acg = ark::readFile(editor + guitarist.gh1Folder + "_ui.acg");
            if (!acg)
                acg = ark::readFile(editor + guitarist.clips() + "_ui.acg");
            // GH1's graph (AnimSet::LoadGraph, GH1 0x182328): rev, clip
            // count, then per clip its jumps: a clip index, from and to beats.
            if (!set || !acp || !acg || u32(*acg, 4u) != 1u)
                return std::nullopt;
            const uint32_t jumps = u32(*acg, 8u);
            if (12u + 12u * jumps > acg->size())
                return std::nullopt;
            Bytes transitions;
            putU32(transitions, 1u);
            putStr(transitions, "ui_loop");
            putU32(transitions, jumps);
            for (uint32_t i = 0; i < jumps; ++i)
                transitions.insert(transitions.end(), acg->begin() + 16 + 12 * i, acg->begin() + 24 + 12 * i);
            const auto idle = readGh1Clip(*acp);
            if (!idle)
                return std::nullopt;
            for (size_t i = 0; i < set->entries.size(); ++i)
            {
                if (set->entries[i].first != "CharClipSamples")
                    continue;
                const auto parts = readGh2Clip(set->bodies[i]);
                if (!parts)
                    return std::nullopt;
                Gh1Clip clip = *idle;
                if (guitarist.pickerDoorOpens)
                    openDoor(clip, *parts);
                set->bodies[i] = writeClip(clip, set->bodies[i], *parts, transitions);
            }
            if (!declareBones(*set, *acp, gh1))
                return std::nullopt;
            return milo::write(*set);
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
        for (const Guitarist &guitarist : kGuitarists)
        {
            const std::string folder = guitarist.gh1Folder, base = guitarist.baseOutfit, outfit = guitarist.outfit();
            const auto gh1 = load("charsys/" + folder + "/gen/" + folder + ".rnd_ps2");
            const auto gh2 = load("char/" + base + "/og/gen/" + base + ".milo_ps2");
            const auto gh2Ui = load("char/" + base + "/og/gen/" + base + "_ui.milo_ps2");
            // The base's _ui plays its picker clips by a path relative to
            // itself: ../../anims/metal1_ui.milo, ../../../goth1/anims/goth1_ui.milo.
            const auto uiClips = gh2Ui ? milo::findSuffix(*gh2Ui, "_ui.milo") : std::nullopt;
            if (!gh1 || !gh2 || !uiClips)
            {
                std::cerr << "[gh1] cannot read " << folder << " or " << base << std::endl;
                continue;
            }
            const std::string file = uiClips->substr(uiClips->rfind('/') + 1);
            const auto picker = pickerSet(guitarist, file.substr(0, file.size() - 8u), *gh1);
            if (!picker)
            {
                std::cerr << "[gh1] cannot build " << folder << "'s picker clips" << std::endl;
                continue;
            }
            milo::Dir ui = graft(*gh2Ui, *gh1);
            milo::replacePrefix(ui, *uiClips, "../../../" + outfit + "/anims/" + outfit + "_ui.milo");
            ark::addFile("char/" + outfit + "/og/gen/" + outfit + ".milo_ps2", milo::write(graft(*gh2, *gh1)));
            ark::addFile("char/" + outfit + "/og/gen/" + outfit + "_ui.milo_ps2", milo::write(ui));
            ark::addFile("char/" + outfit + "/anims/gen/" + outfit + "_ui.milo_ps2", *picker);
            // _horse and anything else beside it is GH2's base outfit's, and
            // so are the photos. The highway (track/surfaces/%s_keep.bmp) is
            // GH1's, which GH2's loader reads as its own.
            ark::rename("char/" + outfit + "/og/gen/" + outfit, *gh2Disc, "char/" + base + "/og/gen/" + base);
            ark::rename("track/surfaces/gen/" + outfit + "_keep", *gh1Disc, "track/surfaces/gen/" + guitarist.track());
            outfits::photosFrom(outfit, *gh2Disc, base);
            outfits::add(guitarist.gh2Character, outfit, base, guitarist.label);
            ++built;
        }
        for (const char *character : kUnnamed)
            outfits::label(character, character, "CLASSIC");
        const auto ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
        std::cerr << "[gh1] " << built << " outfits built in " << ms << " ms" << std::endl;
    }
}
