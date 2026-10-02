// GH1's clips (charsys/<x>/anims) as GH2's CharClipSamples.

#include "gh1/clips.h"

#include "disc/ark.h"
#include "gh1/rig.h"

#include <algorithm>
#include <cctype>
#include <climits>
#include <cmath>
#include <cstring>
#include <functional>
#include <map>
#include <set>
#include <vector>

namespace gh2
{
    namespace
    {
        namespace dtb = gh1::dtb;
        using gh1::bytes;
        using gh1::gh1Bones;
        using gh1::inverse;
        using gh1::kTransLocal;
        using gh1::kTransWorld;
        using gh1::owners;
        using gh1::xfm;
        using gh1::Xfm;
        using milo::Bytes;
        using milo::putStr;
        using milo::putU32;
        using milo::str;
        using milo::u32;

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
            float start = 0.0f, end = 0.0f, rate = 0.0f;
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
            std::memcpy(&clip.rate, b.data() + kClipTiming + 8u, 4u);
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
        bool declareBones(milo::Dir &set, const std::set<std::string> &channels, const milo::Dir &gh1)
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
            // bone_facing is no bone: a clip's facing set takes it
            // (CharClipSamples::FacingSet::Set, 0x16a998).
            auto declared = [](const std::string &bone) { return bone.rfind("bone_facing", 0) != 0; };
            std::vector<std::string> missing;
            for (const std::string &channel : channels)
            {
                const std::string b = channel.substr(0, channel.rfind('.'));
                if (declared(b) && !index(b) && std::find(missing.begin(), missing.end(), b) == missing.end())
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
                if (!declared(channel.substr(0, dot)))
                    continue;
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

        // A macro GH1 defines as a number (kGuitarExtreme, config/macros.dta).
        int32_t macroInt(const dtb::Macros &macros, const std::string &name)
        {
            const auto m = macros.find(name);
            return m != macros.end() && !m->second.empty() && m->second[0].type == dtb::kInt ? m->second[0].integer
                                                                                             : INT32_MIN;
        }

        // A clip in one of GH1's anim sets, its flags numbered.
        struct Gh1Anim
        {
            std::string name;
            std::vector<int32_t> flags;

            bool has(int32_t flag) const { return std::find(flags.begin(), flags.end(), flag) != flags.end(); }
        };

        // An anim set (main::<x>.cset) by its directory (charsys/nu_metal/anims),
        // from the macros GH1's anim scripts define.
        struct Gh1AnimSet
        {
            std::string directory;
            std::vector<Gh1Anim> anims;
        };

        std::optional<Gh1AnimSet> gh1AnimSet(const dtb::Macros &macros, const std::string &directory)
        {
            for (const auto &[macro, body] : macros)
                for (const dtb::Node &cset : body)
                {
                    const dtb::Node *dir = dtb::find(cset, "directory");
                    const dtb::Node *list = dtb::find(cset, "animations");
                    if (!dir || dir->nodes.size() < 2u || dir->nodes[1].text != directory || !list)
                        continue;
                    Gh1AnimSet out{directory, {}};
                    for (size_t i = 1; i < list->nodes.size(); ++i)
                    {
                        const dtb::Node &a = list->nodes[i];
                        if (a.type != dtb::kArray || a.nodes.empty())
                            continue;
                        Gh1Anim anim{a.nodes[0].text, {}};
                        if (const dtb::Node *flags = dtb::find(a, "flags"))
                            for (size_t f = 1; f < flags->nodes.size(); ++f)
                                if (flags->nodes[f].type == dtb::kInt)
                                    anim.flags.push_back(flags->nodes[f].integer);
                        out.anims.push_back(std::move(anim));
                    }
                    return out;
                }
            return std::nullopt;
        }

        // GH1 clips as read, by path.
        struct Gh1Clips
        {
            std::map<std::string, std::optional<Gh1Clip>> read;

            const Gh1Clip *get(const std::string &directory, const std::string &name)
            {
                const std::string path = directory + "/gen/" + name + ".acp";
                auto it = read.find(path);
                if (it == read.end())
                {
                    const auto acp = ark::readFile(path);
                    it = read.emplace(path, acp ? readGh1Clip(*acp) : std::nullopt).first;
                }
                return it->second ? &*it->second : nullptr;
            }
        };

        // A GH2 clip's samples replaced, under the transitions of the clip
        // named (its own when it has none of that name).
        struct Replacement
        {
            Gh1Clip clip;
            std::string transitionsFrom;
        };

        std::optional<Replacement> asIs(Gh1Clips &clips, const std::string &directory, const Gh1Anim &anim)
        {
            const Gh1Clip *clip = clips.get(directory, anim.name);
            return clip ? std::optional(Replacement{*clip, {}}) : std::nullopt;
        }

        // GH1's name for a GH2 clip's motion where they differ: one stop for
        // both feet (stop_left_45_lf_fast), idles and the bad stand without
        // GH2's numbering.
        std::string gh1Name(const std::string &gh2)
        {
            std::string n = gh2;
            if (n.rfind("stop_", 0) == 0)
                for (const char *foot : {"_lf_", "_rf_"})
                    if (const size_t at = n.find(foot); at != std::string::npos)
                        n.erase(at, 3u);
            if (n.rfind("stand_bad_", 0) == 0)
                return "stand_bad";
            if (n.rfind("idle_", 0) == 0 && n.size() > 3u && std::isdigit(static_cast<unsigned char>(n.back())))
                return n.substr(0, n.rfind('_'));
            return n;
        }

        // GH2's moments with their band and stage (band_jump, multi_*), and
        // its holds after a win or loss: GH2's body, GH1's arms.
        bool spliced(const std::string &gh2)
        {
            for (const char *p : {"band_", "multi_", "interactive_", "win_idle", "lose_idle"})
                if (gh2.rfind(p, 0) == 0)
                    return true;
            return false;
        }

        // The GH1 flag a GH2 clip's situation goes by: what a stand-in comes
        // from, and for a splice what its arms come from.
        std::string situation(const std::string &gh2)
        {
            if (spliced(gh2))
                return "kGuitarNormal";
            static const std::pair<const char *, const char *> kSituations[] = {
                {"extreme_", "kGuitarExtreme"}, {"stand_bad", "kGuitarBad"}, {"stand_", "kGuitarNormal"},
                {"idle_", "kGuitarIdle"},       {"intro_", "kGuitarIntro"},  {"win_final", "kGuitarWinFinal"},
                {"win_", "kGuitarWin"},         {"lose_", "kGuitarLose"},    {"special_", "kGuitarSpecial"},
                {"walk_", "kWalkWalk"},         {"turn_", "kWalkTurn"},      {"stop_", "kWalkStop"},
            };
            for (const auto &[p, flag] : kSituations)
                if (gh2.rfind(p, 0) == 0)
                    return flag;
            return {};
        }

        // GH1's strum for GH2's: GH1 has one of each kind, and no bass.
        std::string strum(const std::string &gh2)
        {
            static const std::pair<const char *, const char *> kStrums[] = {
                {"strum_long", "strum_down_long"}, {"strum_short", "strum_pluck_short"},
                {"strum_pick", "strum_pluck_down"}, {"bass_pluck", "strum_pluck_down"},
                {"bass_slap", "strum_down_long"},
            };
            for (const auto &[p, gh1] : kStrums)
                if (gh2.rfind(p, 0) == 0)
                    return gh1;
            return gh2;
        }

        // A clip's channels as tracks: each sample's bytes, one when it
        // holds still.
        using Tracks = std::map<std::string, std::vector<Bytes>>;

        Tracks tracks(const std::vector<Samples> &sets)
        {
            Tracks out;
            for (const Samples &s : sets)
                for (const std::string &c : s.channels)
                    for (uint32_t i = 0; i < s.count; ++i)
                        out[c].push_back(*s.value(c, i));
            return out;
        }

        // Tracks as GH1's two sets: those that move, then those that hold,
        // each in CharBones' type order (position, scale, quat, then the
        // single rotations).
        std::vector<Samples> pack(const Tracks &in, uint32_t count, uint32_t compressed)
        {
            auto rank = [](const std::string &channel)
            {
                const std::string kind = channel.substr(channel.rfind('.') + 1);
                return kind == "pos" ? 0 : kind == "scale" ? 1 : kind == "quat" ? 2 : 3;
            };
            std::vector<std::string> names;
            for (const auto &[name, t] : in)
                names.push_back(name);
            std::stable_sort(names.begin(), names.end(),
                             [&](const std::string &a, const std::string &b) { return rank(a) < rank(b); });
            std::vector<Samples> out(2u);
            out[0].count = count;
            out[1].count = 1u;
            for (Samples &s : out)
                s.compressed = compressed;
            for (const std::string &n : names)
                out[in.at(n).size() > 1u ? 0 : 1].channels.push_back(n);
            for (Samples &s : out)
                for (uint32_t i = 0; i < s.count; ++i)
                    for (const std::string &c : s.channels)
                    {
                        const Bytes &v = in.at(c)[s.count > 1u ? i : 0u];
                        s.data.insert(s.data.end(), v.begin(), v.end());
                    }
            return out;
        }

        // GH2's clip with the arms, the guitar and any hair from GH1's
        // `donor`, which loops by beats across GH2's length. Both must be
        // compressed alike for the bytes to mean the same.
        std::optional<Gh1Clip> splice(const Gh2Clip &gh2, const Gh1Clip &donor)
        {
            const uint32_t compressed = donor.sets.empty() ? 0u : donor.sets[0].compressed;
            uint32_t count = 0u;
            for (const Samples &s : gh2.sets)
            {
                if (!s.channels.empty() && s.compressed != compressed)
                    return std::nullopt;
                if (s.count > 1u && !s.channels.empty())
                    count = s.count;
            }
            for (const Samples &s : donor.sets)
                if (!s.channels.empty() && s.compressed != compressed)
                    return std::nullopt;
            auto arm = [](const std::string &channel)
            {
                for (const char *part : {"clavicle", "upperArm", "foreArm", "Twist", "hand", "index", "middle", "ring",
                                         "pinky", "thumb", "pos_guitar", "hair"})
                    if (channel.find(part) != std::string::npos)
                        return true;
                return false;
            };
            Tracks out = tracks(gh2.sets);
            const float span = donor.end - donor.start;
            for (const auto &[name, track] : tracks(donor.sets))
            {
                if (!arm(name))
                    continue;
                if (track.size() <= 1u || count <= 1u)
                {
                    out[name] = {track[0]};
                    continue;
                }
                std::vector<Bytes> looped;
                for (uint32_t i = 0; i < count; ++i)
                {
                    const float beat = (gh2.end - gh2.start) * static_cast<float>(i) / static_cast<float>(count - 1u);
                    const float at = span > 0.0f ? std::fmod(beat, span) / span : 0.0f;
                    looped.push_back(track[static_cast<size_t>(std::lround(at * static_cast<float>(track.size() - 1u)))]);
                }
                out[name] = std::move(looped);
            }
            return Gh1Clip{gh2.start, gh2.end, gh2.rate, donor.rev, pack(out, count, compressed)};
        }

        // Every clip in `set` that `replace` gives samples for, under GH2's
        // events and the transitions it names; the channels written go into
        // `channels` for the set to declare.
        bool rebuild(milo::Dir &set, std::set<std::string> &channels,
                     const std::function<std::optional<Replacement>(const std::string &, const Gh2Clip &)> &replace)
        {
            std::map<std::string, Bytes> transitions;
            std::vector<std::pair<size_t, Gh2Clip>> parsed;
            for (size_t i = 0; i < set.entries.size(); ++i)
            {
                if (set.entries[i].first != "CharClipSamples")
                    continue;
                auto parts = readGh2Clip(set.bodies[i]);
                if (!parts)
                    return false;
                transitions[set.entries[i].second] =
                    Bytes(set.bodies[i].begin() + kClipTransitions,
                          set.bodies[i].begin() + static_cast<std::ptrdiff_t>(parts->events));
                parsed.emplace_back(i, std::move(*parts));
            }
            for (const auto &[i, parts] : parsed)
            {
                const std::string &name = set.entries[i].second;
                const auto r = replace(name, parts);
                if (!r)
                    continue;
                const auto t = transitions.find(r->transitionsFrom);
                set.bodies[i] = writeClip(r->clip, set.bodies[i], parts,
                                          t != transitions.end() ? t->second : transitions.at(name));
                for (const Samples &s : r->clip.sets)
                    channels.insert(s.channels.begin(), s.channels.end());
            }
            return true;
        }
    }

    namespace gh1
    {
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
            if (!declareBones(*set, clipChannels(*acp), gh1))
                return std::nullopt;
            return milo::write(*set);
        }

        std::optional<SongSets> songSets(const Guitarist &guitarist, const std::string &gh2Set, const milo::Dir &gh1,
                                         const dtb::Macros &macros)
        {
            const std::string anims = std::string("charsys/") + guitarist.gh1Folder + "/anims";
            const auto main = gh1AnimSet(macros, anims);
            const auto hand = gh1AnimSet(macros, anims + "/finger");
            if (!main || !hand)
                return std::nullopt;
            const std::string prefix = guitarist.clips() + "_";
            std::map<std::string, const Gh1Anim *> byName;
            for (const Gh1Anim &a : main->anims)
                byName[a.name.rfind(prefix, 0) == 0 ? a.name.substr(prefix.size()) : a.name] = &a;
            Gh1Clips clips;

            // A stand-in or donor from GH1's clips flagged `flag` at the GH2
            // clip's tempo (fast, or either medium), each in turn.
            std::map<std::pair<int32_t, int>, size_t> turns;
            auto pick = [&](int32_t flag, const std::string &gh2) -> const Gh1Anim *
            {
                const int tempo = gh2.find("fast") != std::string::npos ? 2
                                  : gh2.find("_med") != std::string::npos ? 1 : 0;
                std::vector<const Gh1Anim *> pool, any;
                for (const Gh1Anim &a : main->anims)
                {
                    if (!a.has(flag))
                        continue;
                    any.push_back(&a);
                    const bool fast = a.has(macroInt(macros, "kTempoFast"));
                    const bool medium = a.has(macroInt(macros, "kTempoMedium")) || a.has(macroInt(macros, "kTempoMediumFast"));
                    if ((tempo == 2 && fast) || (tempo == 1 && medium))
                        pool.push_back(&a);
                }
                const auto &from = pool.empty() ? any : pool;
                if (from.empty())
                    return nullptr;
                return from[turns[{flag, tempo}]++ % from.size()];
            };

            auto set = load("char/" + gh2Set + "/anims/gen/" + gh2Set + "_main.milo_ps2");
            std::set<std::string> channels;
            if (!set || !rebuild(*set, channels, [&](const std::string &name, const Gh2Clip &parts) -> std::optional<Replacement>
                {
                    if (const auto a = byName.find(name); a != byName.end())
                        return asIs(clips, main->directory, *a->second);
                    if (const auto a = byName.find(gh1Name(name)); a != byName.end())
                        return asIs(clips, main->directory, *a->second);
                    const std::string kind = situation(name);
                    const Gh1Anim *source = kind.empty() ? nullptr : pick(macroInt(macros, kind), name);
                    const Gh1Clip *donor = source ? clips.get(main->directory, source->name) : nullptr;
                    if (!donor)
                        return std::nullopt;
                    if (spliced(name))
                    {
                        auto clip = splice(parts, *donor);
                        if (!clip)
                            return std::nullopt;
                        return Replacement{std::move(*clip), name};
                    }
                    const std::string own = source->name.rfind(prefix, 0) == 0 ? source->name.substr(prefix.size()) : source->name;
                    return Replacement{*donor, own};
                }) || !declareBones(*set, channels, gh1))
                return std::nullopt;
            SongSets out;
            out.main = milo::write(*set);

            for (const char *kind : {"_fret", "_strum"})
            {
                auto hands = load("char/" + gh2Set + "/anims/gen/" + gh2Set + kind + ".milo_ps2");
                std::set<std::string> handChannels;
                if (!hands || !rebuild(*hands, handChannels, [&](const std::string &name, const Gh2Clip &) -> std::optional<Replacement>
                    {
                        const std::string gh1 = strum(name);
                        for (const Gh1Anim &a : hand->anims)
                            if (a.name == gh1)
                                return asIs(clips, hand->directory, a);
                        return std::nullopt;
                    }) || !declareBones(*hands, handChannels, gh1))
                    return std::nullopt;
                (std::string(kind) == "_fret" ? out.fret : out.strum) = milo::write(*hands);
            }
            return out;
        }
    }
}
