#pragma once

// GH1's clips (charsys/<x>/anims) and GH2's CharClipSamples as they are
// stored, and a GH2 clip set rebuilt around GH1's motion.

#include "formats/dtb.h"
#include "gh1/rig.h"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace gh2
{
    namespace gh1
    {
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

            // A channel holding `value` in every sample, where its type and
            // then its name put it.
            void hold(const std::string &channel, const Bytes &value)
            {
                const auto key = [](const std::string &c)
                {
                    const std::string kind = c.substr(c.rfind('.') + 1);
                    return std::pair(kind == "pos" ? 0 : kind == "scale" ? 1 : kind == "quat" ? 2 : 3, c);
                };
                size_t at = 0u, offset = 0u;
                for (; at < channels.size() && key(channels[at]) < key(channel); ++at)
                    offset += width(channels[at]);
                const size_t s = stride();
                Bytes out;
                for (uint32_t i = 0; i < count; ++i)
                {
                    const auto sample = data.begin() + static_cast<std::ptrdiff_t>(s * i);
                    out.insert(out.end(), sample, sample + static_cast<std::ptrdiff_t>(offset));
                    out.insert(out.end(), value.begin(), value.end());
                    out.insert(out.end(), sample + static_cast<std::ptrdiff_t>(offset), sample + static_cast<std::ptrdiff_t>(s));
                }
                channels.insert(channels.begin() + static_cast<std::ptrdiff_t>(at), channel);
                data = std::move(out);
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

        // GH1's AnimClipSamples (Load, GH1 0x17cdb8) after its class and
        // name: AnimClip rev 17+ (AnimClip::Load, GH1 0x189da0: rev, start,
        // end, rate, flags and two words GH2 has its own of, the second the
        // lead), then the samples' rev and two sets.
        struct Gh1Clip
        {
            float start = 0.0f, end = 0.0f, rate = 0.0f;
            uint32_t rev = 0u;
            std::vector<Samples> sets;
            // How many beats ahead of its event GH1 starts a hand clip:
            // AnimClip +0x20 in memory, 24 bytes on from the rev in the file.
            // GuitarHands::UpdateStrum (GH1 0x286bc8) takes it from the
            // event's beat, against TheGameTime's beat (+0x10, its tick
            // over 480: GameTime::Set, GH1 0x106a70).
            float lead = 0.0f;
        };

        std::optional<Gh1Clip> readGh1Clip(const Bytes &acp);

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

        std::optional<Gh2Clip> readGh2Clip(const Bytes &b);

        // A GH1 clip where GH2's was: GH1's timing and samples at GH1's
        // rev, which GH2's CharBonesSamples still reads as GH1 wrote them,
        // `transitions` (a count, then per clip its name and (from, to) beat
        // pairs), and GH2's flags and events.
        Bytes writeClip(const Gh1Clip &gh1, const Bytes &gh2, const Gh2Clip &parts, const Bytes &transitions);

        // A GH1 clip's channels (bone_x.quat): two sample sets after
        // AnimClip, each a count, names, samples and compression.
        std::set<std::string> clipChannels(const Bytes &acp);

        // A clip set animates only the channels its CharBones declare (bone_x
        // with .trans or .mesh, as the set has it): CharBones::ScaleAdd
        // (0x168320) seeks each clip channel among them without end. One GH1
        // animates that GH2's lacks goes in as its parent's does, at GH1's
        // rest under that parent; each then declares GH1's channel types.
        bool declareBones(milo::Dir &set, const std::set<std::string> &channels, const milo::Dir &gh1);

        // A macro GH1 defines as a number (kGuitarExtreme, config/macros.dta).
        int32_t macroInt(const dtb::Macros &macros, const std::string &name);

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
            std::string name; // x
            std::string directory;
            std::vector<Gh1Anim> anims;
        };

        std::optional<Gh1AnimSet> gh1AnimSet(const dtb::Macros &macros, const std::string &directory);

        // GH1's graph (AnimSet::LoadGraph, GH1 0x17f440): rev, clip count,
        // then per clip its jumps: the clip gone to, from and to beats.
        struct Jump
        {
            uint32_t clip;
            float from, to;
        };

        // A set's graph, <directory>/<name>.acg, a list of jumps per clip.
        std::optional<std::vector<std::vector<Jump>>> graph(const Bytes &acg, size_t clips);

        // A clip's jumps as a GH2 clip's transitions: a count, then per clip
        // gone to its name and (from, to) beat pairs. `plays` is each GH2
        // clip's GH1 anim; a jump goes to every clip playing the one it
        // names.
        Bytes transitions(const std::vector<Jump> &jumps, const std::map<std::string, size_t> &plays);

        // GH1 clips as read, by path.
        struct Gh1Clips
        {
            std::map<std::string, std::optional<Gh1Clip>> read;

            const Gh1Clip *get(const std::string &directory, const std::string &name);
        };

        // A GH2 clip's samples replaced, under `transitions` as writeClip
        // takes them or, with none, those of the clip named (its own when it
        // has none of that name). `blend`, unless 0, is how the clip comes
        // in when a play does not say (its play flags' low four bits,
        // CharClipDriver's constructor, 0x1986d4).
        struct Replacement
        {
            Gh1Clip clip;
            std::string transitionsFrom;
            Bytes transitions;
            uint32_t blend = 0u;
        };

        std::optional<Replacement> asIs(Gh1Clips &clips, const std::string &directory, const Gh1Anim &anim);

        // Every clip in `set` that `replace` gives samples for, under GH2's
        // events and the transitions it names; the channels written go into
        // `channels` for the set to declare.
        bool rebuild(milo::Dir &set, std::set<std::string> &channels,
                     const std::function<std::optional<Replacement>(const std::string &, const Gh2Clip &)> &replace);
    }
}
