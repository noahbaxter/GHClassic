// GH1's clips (charsys/<x>/anims) as GH2's CharClipSamples.

#include "gh1/clips.h"

#include "disc/ark.h"
#include "gh1/clip_set.h"
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
        using gh1::asIs;
        using gh1::clipChannels;
        using gh1::declareBones;
        using gh1::Gh1Anim;
        using gh1::gh1AnimSet;
        using gh1::Gh1Clip;
        using gh1::Gh1Clips;
        using gh1::Gh2Clip;
        using gh1::kClipTransitions;
        using gh1::macroInt;
        using gh1::readGh1Clip;
        using gh1::readGh2Clip;
        using gh1::rebuild;
        using gh1::Replacement;
        using gh1::Samples;
        using gh1::writeClip;
        using milo::Bytes;
        using milo::putStr;
        using milo::putU32;
        using milo::u32;

        // GH2's picker door swings open on its character's clips
        // (bone_door.rotz, through ui_enter, open in ui_loop) as the
        // character walks out. A GH1 idle stands where that walk ends, in
        // the door's swing, so the door stands open from the start where the
        // clip leaves it, a rotation of 1638.4 a radian in an int16 when
        // compressed (CharBones::ScaleAdd, 0x168320). Grim's never opens in
        // GH2, and his GH1 wings would cut through it: his opens as most
        // characters' do, to 4138.
        void openDoor(Gh1Clip &clip, const Gh2Clip &gh2)
        {
            const std::string door = "bone_door.rotz";
            Samples &held = clip.sets[1];
            if (held.count != 1u)
                return;
            float open = 4138.0f / 1638.4f;
            for (const Samples &from : gh2.sets)
                if (const auto last = from.count ? from.value(door, from.count - 1u) : std::nullopt)
                {
                    float end;
                    if (from.compressed)
                    {
                        int16_t s;
                        std::memcpy(&s, last->data(), 2u);
                        end = static_cast<float>(s) / 1638.4f;
                    }
                    else
                        std::memcpy(&end, last->data(), 4u);
                    if (end > 1.0f)
                        open = end;
                }
            Bytes value(held.compressed ? 2u : 4u);
            if (held.compressed)
            {
                const auto s = static_cast<int16_t>(std::lround(open * 1638.4f));
                std::memcpy(value.data(), &s, 2u);
            }
            else
                std::memcpy(value.data(), &open, 4u);            held.appendRotation(door, [&](uint32_t) { return value; });
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
    }

    namespace gh1
    {
        std::optional<Bytes> pickerSet(const Guitarist &guitarist, const std::string &gh2Set, const milo::Dir &gh1)
        {
            const std::string editor = std::string("charsys/") + guitarist.folder + "/anims/editor/";
            auto set = load("char/" + gh2Set + "/anims/gen/" + gh2Set + "_ui.milo_ps2");
            const auto acp = ark::readFile(editor + "gen/" + guitarist.prefix() + "_idle_ui.acp");
            auto acg = ark::readFile(editor + guitarist.folder + "_ui.acg");
            if (!acg)
                acg = ark::readFile(editor + guitarist.prefix() + "_ui.acg");
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
                if (guitarist.doorOpens)
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
            const std::string anims = std::string("charsys/") + guitarist.folder + "/anims";
            const auto main = gh1AnimSet(macros, anims);
            const auto hand = gh1AnimSet(macros, anims + "/finger");
            if (!main || !hand)
                return std::nullopt;
            const std::string prefix = guitarist.prefix() + "_";
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
