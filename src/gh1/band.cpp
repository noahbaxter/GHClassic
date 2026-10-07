#include "gh1/band.h"

#include "content/band.h"
#include "disc/ark.h"
#include "gh1/clip_set.h"
#include "gh1/rig.h"
#include "milo/milo.h"

#include <cstring>
#include <iostream>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace gh2::gh1
{
    namespace
    {
        using milo::putStr;
        using milo::putU32;
        using milo::u32;

        // GH2 has the same five under the same names: charsys/<name> is its
        // char/<name>, playing char/<name>/anims/<set>_main. `face` is GH1's
        // face scene (band_chars.dta's face_file).
        struct Member
        {
            const char *name, *set, *face;
        };
        constexpr Member kMembers[] = {
            {"metal_singer", "singer", "metal_face"},
            {"female_singer", "singer", "face_female_singer"},
            {"metal_bass", "bass", ""},
            {"metal_drummer", "drummer", ""},
            {"metal_keyboard", "keyboard", ""},
        };

        // GH1's graph (AnimSet::LoadGraph, GH1 0x17f440): rev, clip count,
        // then per clip its jumps: the clip gone to, from and to beats.
        struct Jump
        {
            uint32_t clip;
            float from, to;
        };

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

        // The GH1 anim a GH2 clip plays: the one of its name, where GH2
        // numbers its several and GH1's female singer's carry her name, else
        // one for its flags, which are GH1's (kBandActive 4 to kBandWin 256
        // over a tempo, config/macros.dta) and two more: 0x200 on the
        // drummer's nosnare clips, 0x400 on an intro. None for GH2's own
        // moments (band_jump).
        std::optional<size_t> animFor(const Gh1AnimSet &anims, const dtb::Macros &macros, std::string clip,
                                      uint32_t flags)
        {
            const size_t number = clip.find_last_not_of("0123456789");
            if (number != std::string::npos && number + 1u < clip.size() && clip[number] == '_')
                clip.erase(number);
            for (size_t i = 0; i < anims.anims.size(); ++i)
            {
                const std::string &n = anims.anims[i].name;
                if (n == clip || n == "female_" + clip)
                    return i;
            }
            const int32_t active = macroInt(macros, "kBandActive"), idle = macroInt(macros, "kBandIdle");
            int32_t situation = (flags & 0x200u) ? active : (flags & 0x400u) ? idle : INT32_MIN;
            for (const char *flag :
                 {"kBandWin", "kBandLose", "kBandIdle", "kBandActive", "kBandHalf", "kBandDouble", "kBandAllbeat"})
                if (const int32_t f = macroInt(macros, flag); f > 0 && (flags & static_cast<uint32_t>(f)))
                    situation = f;
            std::optional<size_t> any;
            for (size_t i = 0; i < anims.anims.size(); ++i)
            {
                const Gh1Anim &a = anims.anims[i];
                if (!a.has(situation))
                    continue;
                uint32_t tempo = 0u;
                for (const char *flag : {"kTempoMedium", "kTempoFast", "kTempoMediumFast"})
                    if (const int32_t f = macroInt(macros, flag); a.has(f))
                        tempo |= static_cast<uint32_t>(f);
                if (tempo & flags & 3u)
                    return i;
                if (!any)
                    any = i;
            }
            return any;
        }

        // A clip's jumps as a GH2 clip's transitions: a count, then per clip
        // gone to its name and (from, to) beat pairs. `plays` is each GH2
        // clip's GH1 anim; a jump goes to every clip playing the one it
        // names.
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

        std::optional<Bytes> clipSet(const Member &member, const milo::Dir &gh1, const dtb::Macros &macros)
        {
            const std::string name = member.name;
            const auto anims = gh1AnimSet(macros, "charsys/" + name + "/anims");
            const auto acg = anims ? ark::readFile(anims->directory + "/" + anims->name + ".acg") : std::nullopt;
            const auto jumps = acg ? graph(*acg, anims->anims.size()) : std::nullopt;
            auto set = load("char/" + name + "/anims/gen/" + member.set + "_main.milo_ps2");
            if (!jumps || !set)
                return std::nullopt;
            std::map<std::string, size_t> plays;
            for (size_t i = 0; i < set->entries.size(); ++i)
                if (set->entries[i].first == "CharClipSamples")
                    if (const auto anim = animFor(*anims, macros, set->entries[i].second, u32(set->bodies[i], kClipFlags)))
                        plays[set->entries[i].second] = *anim;
            Gh1Clips clips;
            std::set<std::string> channels;
            const auto replace = [&](const std::string &clip, const Gh2Clip &gh2) -> std::optional<Replacement>
            {
                const auto anim = plays.find(clip);
                auto made = anim != plays.end() ? asIs(clips, anims->directory, anims->anims[anim->second]) : std::nullopt;
                if (!made)
                    return made;
                made->transitions = transitions((*jumps)[anim->second], plays);
                // GH1 goes from one clip to another by its graph alone,
                // where the poses meet; GH2 starts a band cue's clip at once
                // unless the clip asks for its first node (kPlayFirst).
                made->blend = 3u;
                // GH2's bassist and singer turn where they stand by their
                // clips' bone_facing, which GH1's stand still without.
                for (const Samples &from : gh2.sets)
                    for (const std::string &channel : from.channels)
                        if (channel.rfind("bone_facing.", 0) == 0)
                        {
                            Samples &held = made->clip.sets[1];
                            held.hold(channel, Bytes(held.width(channel), 0u));
                        }
                return made;
            };
            if (!rebuild(*set, channels, replace) || !declareBones(*set, channels, gh1))
                return std::nullopt;
            return milo::write(*set);
        }
    }

    void addBand(size_t gh2Disc, const dtb::Macros &macros)
    {
        for (const Member &member : kMembers)
        {
            const std::string name = member.name, ours = name + "gh1";
            const std::string theirs = "charsys/" + name + "/gen/", gh2Dir = "char/" + name + "/";
            const auto gh1 = load(theirs + name + ".rnd_ps2");
            const auto face = *member.face ? load(theirs + member.face + ".rnd_ps2") : std::optional(milo::Dir{});
            const auto gh2 = load(gh2Dir + "og/gen/" + name + ".milo_ps2");
            const auto clips = gh1 ? clipSet(member, *gh1, macros) : std::nullopt;
            if (!gh1 || !face || !gh2 || !clips)
            {
                std::cerr << "[gh1] cannot build " << name << std::endl;
                continue;
            }
            milo::Dir outfit = graft(*gh2, *gh1, *face, ours);
            hideUnviewed(outfit, *gh1);
            // GH2's drummer loads the venue's kit (drums.outfit). GH1's kit
            // is its room's (gh1/venues.cpp), so there its drummer has none.
            milo::Dir seated = outfit;
            seated.entries.clear();
            seated.bodies.clear();
            for (size_t i = 0; i < outfit.entries.size(); ++i)
                if (outfit.entries[i].first != "OutfitLoader")
                {
                    seated.entries.push_back(outfit.entries[i]);
                    seated.bodies.push_back(outfit.bodies[i]);
                }
            const std::string home = seated.entries.size() != outfit.entries.size() ? ours + "_room" : std::string();
            for (const std::string &as : {ours, home})
            {
                if (as.empty())
                    continue;
                const std::string dir = "char/" + as + "/";
                ark::addFile(dir + "og/gen/" + as + ".milo_ps2", milo::write(as == ours ? outfit : seated));
                // The outfit names its clips beside itself (../../anims/<set>_main.milo).
                ark::addFile(dir + "anims/gen/" + member.set + "_main.milo_ps2", *clips);
                // The rest is GH2's: the horse head's outfit, the singers' visemes.
                ark::rename(dir + "og/gen/" + as, gh2Disc, gh2Dir + "og/gen/" + name);
                ark::rename(dir, gh2Disc, gh2Dir);
            }
            band::add("gh1", name, ours, home);
        }
    }
}
