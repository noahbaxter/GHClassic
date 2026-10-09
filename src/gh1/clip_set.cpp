#include "gh1/clip_set.h"

#include "disc/ark.h"

#include <algorithm>
#include <cstring>

namespace gh2
{
    namespace gh1
    {
        using milo::putStr;
        using milo::putU32;
        using milo::str;
        using milo::u32;

        namespace
        {
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
        }

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
            std::memcpy(&clip.lead, acp.data() + o + 24u, 4u); // AnimClip +0x20
            clip.rev = u32(acp, o + 28u);
            auto sets = readSamples(acp, o + 32u, clip.rev, 2);
            if (!sets)
                return std::nullopt;
            clip.sets = std::move(*sets);
            return clip;
        }

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
                    milo::add(set, "CharBone", *it + suffix, std::move(body));
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

        int32_t macroInt(const dtb::Macros &macros, const std::string &name)
        {
            const auto m = macros.find(name);
            return m != macros.end() && !m->second.empty() && m->second[0].type == dtb::kInt ? m->second[0].integer
                                                                                             : INT32_MIN;
        }

        std::optional<Gh1AnimSet> gh1AnimSet(const dtb::Macros &macros, const std::string &directory)
        {
            for (const auto &[macro, body] : macros)
                for (const dtb::Node &cset : body)
                {
                    const dtb::Node *dir = dtb::find(cset, "directory");
                    const dtb::Node *list = dtb::find(cset, "animations");
                    if (!dir || dir->nodes.size() < 2u || dir->nodes[1].text != directory || !list)
                        continue;
                    const std::string &full = cset.nodes[0].text; // main::bass.cset
                    const size_t scope = full.find("::");
                    const size_t from = scope == std::string::npos ? 0u : scope + 2u;
                    Gh1AnimSet out{full.substr(from, full.rfind('.') - from), directory, {}};
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

        std::optional<std::vector<std::vector<Jump>>> graph(const Bytes &acg, size_t clips)
        {
            if (u32(acg, 0u) != 1u || u32(acg, 4u) != clips)
                return std::nullopt;
            std::vector<std::vector<Jump>> out(clips);
            size_t o = 8u;
            for (std::vector<Jump> &jumps : out)
            {
                const uint32_t n = u32(acg, o);
                o += 4u;
                if (o + 12u * n > acg.size())
                    return std::nullopt;
                for (uint32_t i = 0; i < n; ++i, o += 12u)
                {
                    Jump jump{u32(acg, o), 0.0f, 0.0f};
                    std::memcpy(&jump.from, acg.data() + o + 4u, 4u);
                    std::memcpy(&jump.to, acg.data() + o + 8u, 4u);
                    if (jump.clip >= clips)
                        return std::nullopt;
                    jumps.push_back(jump);
                }
            }
            return out;
        }

        Bytes transitions(const std::vector<Jump> &jumps, const std::map<std::string, size_t> &plays)
        {
            std::map<std::string, std::vector<Jump>> to;
            for (const Jump &jump : jumps)
                for (const auto &[clip, anim] : plays)
                    if (anim == jump.clip)
                        to[clip].push_back(jump);
            Bytes out;
            putU32(out, static_cast<uint32_t>(to.size()));
            for (const auto &[clip, pairs] : to)
            {
                putStr(out, clip);
                putU32(out, static_cast<uint32_t>(pairs.size()));
                for (const Jump &jump : pairs)
                    for (const float beat : {jump.from, jump.to})
                    {
                        uint32_t bits;
                        std::memcpy(&bits, &beat, 4u);
                        putU32(out, bits);
                    }
            }
            return out;
        }

        const Gh1Clip *Gh1Clips::get(const std::string &directory, const std::string &name)
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

        std::optional<Replacement> asIs(Gh1Clips &clips, const std::string &directory, const Gh1Anim &anim)
        {
            const Gh1Clip *clip = clips.get(directory, anim.name);
            return clip ? std::optional(Replacement{*clip, {}}) : std::nullopt;
        }

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
                                          !r->transitions.empty() ? r->transitions
                                          : t != transitions.end() ? t->second : transitions.at(name));
                if (r->blend != 0u)
                    set.bodies[i][kClipFlags + 4u] = static_cast<uint8_t>((set.bodies[i][kClipFlags + 4u] & 0xf0u) | r->blend);
                for (const Samples &s : r->clip.sets)
                    channels.insert(s.channels.begin(), s.channels.end());
            }
            return true;
        }
    }
}
