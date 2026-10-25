#include "hub/menu_guitarist.h"

#include "content/games.h"
#include "disc/ark.h"
#include "dta.h"
#include "formats/dtb.h"
#include "gh1/clip_set.h"
#include "gh1/scene.h"
#include "gh2x/models.h"
#include "guest.h"
#include "hook.h"
#include "milo/milo.h"
#include "ps2_runtime_macros.h"
#include "script.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <vector>
#include <string>
#include <string_view>
#include <tuple>

namespace gh2::menu_guitarist
{
    namespace
    {
        // The pre-show's clips, all kPreshow's: kPick's, picked by hand
        // (preshow_picks.dta), and kIdle, the set's stillest idle
        // (kAnyIdle), which each hands over to at its end. The sandbox's
        // gallery weighs the song's walk-ons (kIntro) and interludes
        // (kInterludes) too.
        constexpr const char *kIntro = "intro_";
        constexpr const char *kInterludes[] = {"stand_", "extreme_", "multi_", "win_"};
        constexpr const char *kBad = "_bad"; // playing badly: not for a show
        constexpr const char *kPreshow = "preshow_";
        constexpr const char *kIdle = "preshow_idle";
        constexpr const char *kAnyIdle = "idle_";
        // A clip's hand IK flags: each hand held to its grip on the guitar
        // (fret.ik, strum), which a clip that lets go of it leaves off.
        constexpr uint32_t kFretHand = 0x00400000u, kStrumHand = 0x00800000u, kBothHands = kFretHand | kStrumHand;
        // Picked by hand (preshow_picks.dta): the walk-on, then the loop.
        constexpr const char *kPick = "preshow_pick_";
        constexpr const char *kMain = "_main.milo_ps2";
        // A clip's default play flags follow its flags (CharClip): its time
        // ones (kPlayRealTime 0x200, kPlayUserTime 0x400, kPlayBeatAlign*
        // 0x1000 to 0x8000). A song's idles wait for a beat, which never
        // comes outside a song; its walk-ons play in real time.
        constexpr size_t kPlayFlags = gh1::kClipFlags + 4u;
        constexpr uint32_t kTimeFlags = 0xff00u, kRealTime = 0x200u;
        // A clip's tempo (its flags' and its set row's low bits: 1 medium, 2
        // fast, 0 either): a set loads only the clips of the tempo its
        // figure loads at, which outside a song is whatever the last was.
        // The pre-show's copies have none, so are always there.
        constexpr uint32_t kTempoFlags = 0x3u;

        // A set's own object keeps a row per clip, as many as the dir has
        // and by name (CharClipSet::Save, 0x16d808): its name, its flags and
        // the size it takes loaded. Where the row of `name` starts, if any.
        std::optional<size_t> rowOf(const milo::Bytes &root, const std::string &name)
        {
            milo::Bytes key;
            milo::putStr(key, name);
            const auto at = std::search(root.begin(), root.end(), key.begin(), key.end());
            if (at == root.end())
                return std::nullopt;
            return static_cast<size_t>(at - root.begin());
        }

        // Where a walk-on leaves for an idle in a song: its latest
        // transition's beat, short of its end, which is never reached. Its
        // earlier ones are a song cutting it short.
        float leaveBeat(const milo::Bytes &b)
        {
            float leave = gh1::f32(b, gh1::kClipTiming + 4u) - 0.5f;
            size_t p = gh1::kClipTransitions;
            const uint32_t count = milo::u32(b, p);
            p += 4u;
            float latest = -1e9f;
            for (uint32_t i = 0; i < count; ++i)
            {
                milo::str(b, p);
                const uint32_t pairs = milo::u32(b, p);
                for (uint32_t j = 0; j < pairs; ++j)
                    latest = std::max(latest, gh1::f32(b, p + 4u + 8u * j));
                p += 4u + 8u * pairs;
            }
            return latest > -1e9f ? latest : leave;
        }

        // Where an idle goes round in a song: its transition back to itself
        // (leave, start), short of its end, whose tail turns away to
        // whatever comes next. Without one, from half a beat short of its
        // end to its start.
        std::pair<float, float> selfLoop(const milo::Bytes &b, const std::string &name)
        {
            size_t p = gh1::kClipTransitions;
            const uint32_t count = milo::u32(b, p);
            p += 4u;
            for (uint32_t i = 0; i < count; ++i)
            {
                const std::string next = milo::str(b, p);
                const uint32_t pairs = milo::u32(b, p);
                if (next == name && pairs > 0u)
                    return {gh1::f32(b, p + 4u), gh1::f32(b, p + 8u)};
                p += 4u + 8u * pairs;
            }
            return {gh1::f32(b, gh1::kClipTiming + 4u) - 0.5f, gh1::f32(b, gh1::kClipTiming)};
        }

        // A clip's transitions: none, or one to `next`, from `leave` to its
        // `start`.
        milo::Bytes transitions(const std::string &next = "", float leave = 0.0f, float start = 0.0f)
        {
            milo::Bytes out;
            milo::putU32(out, next.empty() ? 0u : 1u);
            if (next.empty())
                return out;
            milo::putStr(out, next);
            milo::putU32(out, 1u);
            gh1::putF32(out, leave);
            gh1::putF32(out, start);
            return out;
        }

        // The root (bone_facing.pos), whose moves across the floor the
        // driver keeps from clip to clip, and the hips (bone_pelvis.pos) off
        // it. x across, y toward the camera, z up.
        constexpr const char *kRoot = "bone_facing.pos", *kHips = "bone_pelvis.pos";
        std::array<float, 3> posAt(const gh1::Samples &s, const char *channel, uint32_t sample)
        {
            std::array<float, 3> xyz{};
            if (const auto v = s.value(channel, sample))
                std::memcpy(xyz.data(), v->data(), 12u);
            return xyz;
        }

        // The sample a walk-on hands over to the idle at, `leave` beats in.
        uint32_t sampleAt(const gh1::Gh2Clip &parts, const gh1::Samples &s, float leave)
        {
            if (s.count < 2u || parts.end <= parts.start)
                return s.count ? s.count - 1u : 0u;
            const float t = std::clamp((leave - parts.start) / (parts.end - parts.start), 0.0f, 1.0f);
            return static_cast<uint32_t>(std::lround(t * float(s.count - 1u)));
        }

        // How much of a clip walks as it was made before fitRoot starts
        // taking it back to its spot: its walks show, and only its tail
        // gives the ground back.
        constexpr float kWalkFree = 0.6f;
        // Hips this far over where they start: off the ground, a jump.
        constexpr float kAir = 3.0f;
        // Where the root has gone by `sample`, shed up to `until` (fitRoot):
        // back on its spot there. None shed in its first kWalkFree, then as
        // it moves, by how far it has come along its way since, none while
        // it stands: shed evenly over the whole clip, a walk toward the
        // camera walked on the spot and a figure standing still slid.
        std::array<float, 2> rootOff(const gh1::Samples &s, uint32_t sample, uint32_t until)
        {
            const auto a = posAt(s, kRoot, 0u), b = posAt(s, kRoot, until), p = posAt(s, kRoot, sample);
            // A clip that leaves the ground slides only while it is up
            // (glam1's win_01 jump): by how high its hips are past kAir.
            const float ground = posAt(s, kHips, 0u)[2];
            float air = 0.0f, airSoFar = 0.0f;
            for (uint32_t i = 1u; i <= until && i < s.count; ++i)
            {
                const float up = std::max(0.0f, posAt(s, kHips, i)[2] - ground - kAir);
                air += up;
                if (i <= sample)
                    airSoFar += up;
            }
            if (air > 0.0f)
            {
                const float f = std::min(1.0f, airSoFar / air);
                return {p[0] - a[0] - f * (b[0] - a[0]), p[1] - a[1] - f * (b[1] - a[1])};
            }
            const uint32_t from = static_cast<uint32_t>(float(until) * kWalkFree);
            float sofar = 0.0f, whole = 0.0f;
            for (uint32_t i = from + 1u; i <= until && i < s.count; ++i)
            {
                const auto last = posAt(s, kRoot, i - 1u), next = posAt(s, kRoot, i);
                const float step = std::hypot(next[0] - last[0], next[1] - last[1]);
                whole += step;
                if (i <= sample)
                    sofar += step;
            }
            const float f = sample <= from ? 0.0f
                            : whole > 0.0f ? std::min(1.0f, sofar / whole)
                                           : float(sample - from) / float(std::max(1u, until - from));
            return {p[0] - a[0] - f * (b[0] - a[0]), p[1] - a[1] - f * (b[1] - a[1])};
        }

        // How far a walk-on takes the figure from its mark until it hands
        // over at `leave`, its root put back on the mark by then (fitRoot):
        // the hips' farthest across (x) and back and forth (y), and up or
        // down past kBob (a win's last leap ends hundreds up). Wild for a
        // broken clip (alterna1's win_final): too far, either way.
        constexpr float kBob = 12.0f;
        struct Reach
        {
            float left = 0.0f, right = 0.0f, back = 0.0f, forward = 0.0f, up = 0.0f;
            // How far its root ends up from where it started by the handover,
            // which fitRoot takes away bit by bit (its feet slide that far),
            // and goes at most (held, its feet step on the spot).
            float drift = 0.0f, roam = 0.0f;
        };
        Reach reach(const gh1::Gh2Clip &parts, float leave, bool held = false)
        {
            const gh1::Samples &s = parts.sets.front();
            const uint32_t until = sampleAt(parts, s, leave);
            const auto hips0 = posAt(s, kHips, 0u);
            const auto root0 = posAt(s, kRoot, 0u), rootEnd = posAt(s, kRoot, until);
            Reach r;
            r.drift = std::hypot(rootEnd[0] - root0[0], rootEnd[1] - root0[1]);
            for (uint32_t i = 0; i <= until && i < s.count; ++i)
            {
                const auto p = posAt(s, kRoot, i);
                r.roam = std::max(r.roam, std::hypot(p[0] - root0[0], p[1] - root0[1]));
                const auto root = held ? std::array<float, 2>{} : rootOff(s, i, until);
                const auto hips = posAt(s, kHips, i);
                const float x = root[0] + hips[0] - hips0[0], y = root[1] + hips[1] - hips0[1];
                r.left = std::max(r.left, -x), r.right = std::max(r.right, x);
                r.back = std::max(r.back, -y), r.forward = std::max(r.forward, y);
                if (std::fabs(hips[2] - hips0[2]) > kBob)
                    r.up = std::max(r.up, std::fabs(hips[2] - hips0[2]));
            }
            return r;
        }

        // Feet that slide or step on the spot past this show: an idle, held
        // still, may walk no further.
        constexpr float kSlide = 1.0f;

        // A bone's turn (quat channel `c`) in sample `i`.
        std::array<float, 4> quatAt(const gh1::Samples &s, const std::string &c, uint32_t i)
        {
            std::array<float, 4> q{};
            const auto v = s.value(c, i);
            for (size_t k = 0; v && k < 4u; ++k)
                if (s.compressed)
                {
                    int16_t h;
                    std::memcpy(&h, v->data() + 2u * k, 2u);
                    q[k] = float(h) / 32767.0f;
                }
                else
                    std::memcpy(&q[k], v->data() + 4u * k, 4u);
            return q;
        }

        // How far apart two of a bone's turns are, as 4-vectors, either sign.
        float turnGap(const std::array<float, 4> &a, const std::array<float, 4> &b)
        {
            float same = 0.0f, flipped = 0.0f;
            for (size_t k = 0; k < 4u; ++k)
                same += (a[k] - b[k]) * (a[k] - b[k]), flipped += (a[k] + b[k]) * (a[k] + b[k]);
            return std::sqrt(std::min(same, flipped));
        }

        // How much a clip moves the figure: its bones' turn from sample to
        // sample, on average.
        float restlessness(const gh1::Gh2Clip &parts)
        {
            const gh1::Samples &s = parts.sets.front();
            if (s.count < 2u)
                return 0.0f;
            float sum = 0.0f;
            for (const std::string &c : s.channels)
            {
                if (!c.ends_with(".quat"))
                    continue;
                for (uint32_t i = 1; i < s.count; ++i)
                    sum += turnGap(quatAt(s, c, i - 1u), quatAt(s, c, i));
            }
            return sum / float(s.count - 1u);
        }

        // Where an idle round best leaves for its own `start`: of the beats
        // in its second half short of `latest`, the one whose pose is nearest
        // start's (every bone's turn), so that one round goes on into the
        // next without a jump. Its samples lie evenly from its first beat to
        // its last.
        float bestLoop(const milo::Bytes &b, const gh1::Gh2Clip &parts, float start, float latest)
        {
            const gh1::Samples &s = parts.sets.front();
            const float first = gh1::f32(b, gh1::kClipTiming), last = gh1::f32(b, gh1::kClipTiming + 4u);
            if (s.count < 3u || last <= first)
                return latest;
            const float step = (last - first) / float(s.count - 1u);
            const auto sampleAt = [&](float beat)
            {
                return static_cast<uint32_t>(std::clamp(std::lround((beat - first) / step), 0l, long(s.count - 1u)));
            };
            const uint32_t from = sampleAt(start);
            float best = latest, least = 1e9f;
            for (uint32_t i = sampleAt((start + latest) / 2.0f); i <= sampleAt(latest); ++i)
            {
                float gap = 0.0f;
                for (const std::string &c : s.channels)
                    if (c.ends_with(".quat"))
                        gap += turnGap(quatAt(s, c, from), quatAt(s, c, i));
                if (gap < least)
                    least = gap, best = first + step * float(i);
            }
            return best;
        }

        // `clip`'s root back where it starts across the floor: the driver
        // keeps what a clip moves it, and played round and round even a
        // still clip would walk it off. With `leave`, a walk-on's: it walks
        // as it does, its root shed of where it ends up bit by bit, back on
        // its spot by the time it hands over to the idle `leave` beats in.
        // Without, kept where it starts throughout (the idle). Its sets' data
        // end the clip, in order.
        // An `entrance` is never slid: all of it moved by where it ends up,
        // it walks in from wherever its walk starts and stops on the spot.
        void fitRoot(milo::Bytes &clip, const gh1::Gh2Clip &parts, std::optional<float> leave, bool entrance)
        {
            size_t total = 0u;
            for (const gh1::Samples &s : parts.sets)
                total += s.stride() * s.count;
            if (total > clip.size())
                return;
            size_t o = clip.size() - total;
            for (const gh1::Samples &s : parts.sets)
            {
                if (!std::equal(s.data.begin(), s.data.end(), clip.begin() + static_cast<std::ptrdiff_t>(o)))
                    return;
                const auto first = posAt(s, kRoot, 0u);
                const uint32_t until = leave ? sampleAt(parts, s, *leave) : 0u;
                const auto end = posAt(s, kRoot, until);
                size_t channel = 0u;
                for (const std::string &c : s.channels)
                {
                    if (c == kRoot)
                        for (uint32_t i = 0; i < s.count; ++i)
                        {
                            const auto p = posAt(s, kRoot, i);
                            const auto off = entrance ? std::array<float, 2>{p[0] - end[0], p[1] - end[1]}
                                                      : rootOff(s, i, until);
                            const float xy[2] = {first[0] + (leave ? off[0] : 0.0f), first[1] + (leave ? off[1] : 0.0f)};
                            std::memcpy(clip.data() + o + s.stride() * i + channel, xy, 8u);
                        }
                    channel += s.width(c);
                }
                o += s.stride() * s.count;
            }
        }

        // Clip `name` of `set` again as `as`, with no events and `moves` for
        // its transitions, in real time, its root fitted (fitRoot) and its row
        // among the set's in name order.
        bool addClip(milo::Dir &set, const std::string &name, const std::string &as, const milo::Bytes &moves,
                     std::optional<float> leave, uint32_t addFlags = 0u, bool entrance = false)
        {
            const auto at = milo::find(set, name);
            const auto source = rowOf(set.root, name);
            if (!at || !source || set.entries[*at].first != "CharClipSamples")
                return false;
            const milo::Bytes &b = set.bodies[*at];
            const auto parts = gh1::readGh2Clip(b);
            if (!parts)
                return false;
            milo::Bytes clip(b.begin(), b.begin() + gh1::kClipTransitions);
            const uint32_t clipFlags = (milo::u32(clip, gh1::kClipFlags) & ~kTempoFlags) | addFlags;
            std::memcpy(clip.data() + gh1::kClipFlags, &clipFlags, 4u);
            const uint32_t flags = (milo::u32(clip, kPlayFlags) & ~kTimeFlags) | kRealTime;
            std::memcpy(clip.data() + kPlayFlags, &flags, 4u);
            clip.insert(clip.end(), moves.begin(), moves.end());
            milo::putStr(clip, "");
            milo::putStr(clip, "");
            milo::putU32(clip, 0u);
            clip.insert(clip.end(), b.begin() + static_cast<std::ptrdiff_t>(parts->eventsEnd), b.end());
            fitRoot(clip, *parts, leave, entrance);

            std::vector<std::string> clips;
            for (const auto &[cls, n] : set.entries)
                if (cls == "CharClipSamples")
                    clips.push_back(n);
            std::sort(clips.begin(), clips.end());
            const auto after = std::upper_bound(clips.begin(), clips.end(), as);
            std::optional<size_t> row;
            if (after != clips.end())
                row = rowOf(set.root, *after);
            else if (const auto last = rowOf(set.root, clips.back()))
                row = *last + 4u + clips.back().size() + 8u;
            if (!row)
                return false;
            milo::Bytes values;
            milo::putStr(values, as);
            const size_t from = *source + 4u + name.size();
            values.insert(values.end(), set.root.begin() + static_cast<std::ptrdiff_t>(from),
                          set.root.begin() + static_cast<std::ptrdiff_t>(from + 8u));
            std::memcpy(values.data() + values.size() - 8u, &clipFlags, 4u);
            set.root.insert(set.root.begin() + static_cast<std::ptrdiff_t>(*row), values.begin(), values.end());
            milo::add(set, "CharClipSamples", as, std::move(clip));
            return true;
        }

        // Set `key`'s picks, (walk_on <clip>) and (loop <clip>..), if picked.
        const dtb::Node *picksFor(const std::string &key)
        {
            static const std::optional<dtb::Node> picks = dtb::parse(kPreshowPicksDta);
            return picks ? dtb::find(*picks, key) : nullptr;
        }
    }

    bool addWalkOns(milo::Dir &main, const std::string &game)
    {
        if (milo::find(main, kIdle))
            return true;
        // Picked by hand, from the set's own clips only; a set with none of
        // its own (a custom character's) has the default's.
        const std::string key = game + "_" + main.name.substr(0, main.name.rfind("_main"));
        const dtb::Node *picks = picksFor(key);
        if (!picks)
            picks = picksFor("default");
        if (!picks)
            return false;
        const auto clipAt = [&](const std::string &name) -> std::optional<size_t>
        {
            const auto at = milo::find(main, name);
            if (!at || main.entries[*at].first != "CharClipSamples" || main.bodies[*at].size() <= gh1::kClipTiming + 4u)
                return std::nullopt;
            return at;
        };
        std::vector<std::string> idles;
        for (size_t i = 0; i < main.entries.size(); ++i)
            if (main.entries[i].second.starts_with(kAnyIdle) && clipAt(main.entries[i].second))
                idles.push_back(main.entries[i].second);
        std::sort(idles.begin(), idles.end());
        if (idles.empty())
            return false;
        // The idle: one that keeps both hands on the guitar (its flags'
        // both-hands IK, 0xc00000), then a medium song's over a fast one's
        // (idle_fast_* bobs at a fast song's tempo), then the stillest; never
        // one that walks (rock1's idle_medium_02, 10 across), held still it
        // slides.
        std::string idle;
        std::tuple<bool, bool, float> best{false, false, -1e9f};
        for (const std::string &name : idles)
        {
            const milo::Bytes &b = main.bodies[*milo::find(main, name)];
            const auto parts = gh1::readGh2Clip(b);
            if (!parts || parts->sets.empty() || reach(*parts, leaveBeat(b)).roam > kSlide)
                continue;
            const bool both = (milo::u32(b, gh1::kClipFlags) & kBothHands) == kBothHands;
            const std::tuple<bool, bool, float> score{both, name.find("medium") != std::string::npos,
                                                      -restlessness(*parts)};
            if (idle.empty() || score > best)
                idle = name, best = score;
        }
        if (idle.empty())
            idle = idles.front();
        // Or the one picked by hand, (idle <clip>): any of the set's.
        if (const dtb::Node *own = dtb::find(*picks, "idle"); own && own->nodes.size() > 1u)
        {
            if (clipAt(own->nodes[1].text))
                idle = own->nodes[1].text;
            else
                std::cerr << "[menu_guitarist] " << key << " has no idle " << own->nodes[1].text << std::endl;
        }
        const milo::Bytes &idleBody = main.bodies[*milo::find(main, idle)];
        const float idleStart = gh1::f32(idleBody, gh1::kClipTiming);
        // Round itself by its own way back to itself, blended, never
        // reaching its tail.
        auto [loopLeave, loopStart] = selfLoop(idleBody, idle);
        if (const auto parts = gh1::readGh2Clip(idleBody); parts && !parts->sets.empty())
            loopLeave = bestLoop(idleBody, *parts, loopStart, loopLeave);
        if (!addClip(main, idle, kIdle, transitions(kIdle, loopLeave, loopStart), std::nullopt))
            return false;
        // The idle's root is held still throughout: how far it moved.
        if (std::getenv("GHC_PRESHOW_GALLERY"))
            if (const auto parts = gh1::readGh2Clip(main.bodies[*milo::find(main, idle)]); parts && !parts->sets.empty())
                std::cerr << "[preshow idle] " << key << " " << idle << " roam " << reach(*parts, loopLeave, true).roam
                          << std::endl;
        // Picked by hand: each in turn, (fret <clip>) its fretting hand held
        // to the neck, (hold <clip>) both hands to their grips.
        {
            std::vector<const dtb::Node *> order;
            // Clip 1 plays once, then 2 to the last round and round
            // (preshow_guitarist.dta). With no walk-on the loop's first is
            // clip 1 and comes round again last, so none plays twice running.
            const dtb::Node *loop = dtb::find(*picks, "loop");
            const dtb::Node *walkOn = dtb::find(*picks, "walk_on");
            const bool own = walkOn && walkOn->nodes.size() > 1u;
            if (own)
                order.push_back(&walkOn->nodes[1]);
            if (loop)
                for (size_t i = 1; i < loop->nodes.size(); ++i)
                    order.push_back(&loop->nodes[i]);
            if (!own && !order.empty())
                order.push_back(order.front());
            size_t n = 0;
            for (const dtb::Node *pick : order)
            {
                const bool wrapped = pick->type == dtb::kArray && pick->nodes.size() >= 2u;
                const std::string mode = wrapped ? pick->nodes[0].text : "";
                const uint32_t hands = mode == "fret" ? kFretHand : mode == "hold" ? kBothHands : 0u;
                const std::string &picked = wrapped ? pick->nodes.back().text : pick->text;
                const std::string name = picked == "idle" ? idle : picked;
                const auto at = clipAt(name);
                const float leave = at ? leaveBeat(main.bodies[*at]) : 0.0f;
                // The walk-on walks in, never slid (fitRoot).
                if (at && addClip(main, name, kPick + std::to_string(n + 1u), transitions(kIdle, leave, idleStart),
                                  leave, hands, own && n == 0u))
                {
                    ++n;
                    // How far fitRoot slides it back over its last part, for
                    // reviewing clips.
                    if (std::getenv("GHC_PRESHOW_GALLERY"))
                        if (const auto parts = gh1::readGh2Clip(main.bodies[*at]); parts && !parts->sets.empty())
                        {
                            const Reach r = reach(*parts, leave);
                            const gh1::Samples &s = parts->sets.front();
                            float up = 0.0f;
                            for (uint32_t i = 0; i <= sampleAt(*parts, s, leave) && i < s.count; ++i)
                                up = std::max(up, posAt(s, kHips, i)[2] - posAt(s, kHips, 0u)[2]);
                            std::cerr << "[preshow pick] " << key << " " << n << " " << picked << " drift " << r.drift
                                      << " roam " << r.roam << " free " << kWalkFree << " up " << up << std::endl;
                        }
                }
                else
                    std::cerr << "[menu_guitarist] " << key << " has no clip " << picked << std::endl;
            }
        }
        // For the sandbox (tools/ghc.py sandbox): every walk-on and riff the
        // pre-show weighed, as it would show them, again as preshow_all_<name>.
        // Its wins' idles, last leaps and losses too, for a look at them.
        if (std::getenv("GHC_PRESHOW_GALLERY"))
        {
            for (size_t i = 0; i < main.entries.size(); ++i)
            {
                const std::string name = main.entries[i].second;
                // Each clip's drift and roam (reach), for reviewing clips.
                if (main.entries[i].first == "CharClipSamples" && !name.starts_with(kPreshow))
                    if (const auto parts = gh1::readGh2Clip(main.bodies[i]); parts && !parts->sets.empty())
                    {
                        const Reach r = reach(*parts, leaveBeat(main.bodies[i]));
                        std::cerr << "[preshow clip] " << key << " " << name << " drift " << r.drift << " roam "
                                  << r.roam << " hands " << ((milo::u32(main.bodies[i], gh1::kClipFlags) & kBothHands) >> 22)
                                  << std::endl;
                    }
                if (main.entries[i].first != "CharClipSamples" || name.starts_with(kPreshow) ||
                    name.find(kBad) != std::string::npos ||
                    !(name.starts_with(kIntro) || name.starts_with("lose_") ||
                      std::any_of(std::begin(kInterludes), std::end(kInterludes),
                                  [&](const char *k) { return name.starts_with(k); })))
                    continue;
                const float leave = leaveBeat(main.bodies[i]);
                addClip(main, name, "preshow_all_" + name, transitions(kIdle, leave, idleStart), leave);
            }
        }
        return true;
    }

    namespace
    {
        // An outfit's song figure, as the pre-show names it (onFilePathSet).
        constexpr std::string_view kSongPrefix = "ghcsong_";

        const Addresses *s_addresses = nullptr;
        PS2Runtime::RecompiledFunction s_getClipGroup = nullptr;
        constexpr uint32_t kPlayerNum = 0x270u, kIdleGroup = 0x274u; // GuitarCharacter
        constexpr uint32_t kPlayer = 0xb4u;                           // PlayerConfig: its Player

        // GuitarCharacter::GetClipGroup (0x10b7b0): its idle group, or while
        // its Player does badly its bad. Outside a song, the pre-show's,
        // there is no Player: its idle.
        void getClipGroup(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t self = GPR_U32(ctx, 4);
            const R5900Context saved = *ctx;
            const uint32_t config = static_cast<uint32_t>(runtime->callGuestFunction(
                rdram, ctx, s_addresses->gameConfigPlayerConfig,
                {load<uint32_t>(rdram, s_addresses->theGameConfig), load<uint32_t>(rdram, self + kPlayerNum)}));
            *ctx = saved;
            if (config && load<uint32_t>(rdram, config + kPlayer))
                return s_getClipGroup(rdram, ctx, runtime);
            SET_GPR_U32(ctx, 2, load<uint32_t>(rdram, self + kIdleGroup));
            ctx->pc = GPR_U32(ctx, 31);
        }

        // The pre-show's preview panel loads an outfit's figure as
        // char/%s/og/%s_ui.milo (CharsysPanel::CacheModel::PollLoading,
        // 0x1413a0). Under kSongPrefix that is its song figure's path,
        // char/<outfit>/og/<outfit>.milo, as a song loads it from wherever it
        // is served (the disc's, the 360's, GH1's): rewritten in place in the
        // string FilePath::Set takes, which is the shorter.
        struct FilePathSetTag;
        void onFilePathSet(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            const uint32_t at = GPR_U32(ctx, 6);
            if (!at)
                return;
            char *path = reinterpret_cast<char *>(getMemPtr(rdram, at));
            constexpr std::string_view kHead = "char/", kTail = "_ui.milo";
            const std::string_view view(path);
            if (!view.starts_with(std::string(kHead) + std::string(kSongPrefix)) || !view.ends_with(kTail))
                return;
            const std::string_view rest = view.substr(kHead.size() + kSongPrefix.size());
            const std::string outfit(rest.substr(0, rest.find('/')));
            const std::string song = std::string(kHead) + outfit + "/og/" + outfit + ".milo";
            std::memcpy(path, song.c_str(), song.size() + 1u);
        }

        // CharsysPanel::ValidChar (0x142bc0): an outfit the career has
        // unlocked. A song figure's name is no career item: valid as its
        // outfit is shown, which the pre-show asks only of one picked.
        struct ValidCharTag;
        bool validChar(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            const uint32_t name = GPR_U32(ctx, 5);
            if (!name || !std::string_view(reinterpret_cast<const char *>(getMemPtr(rdram, name))).starts_with(kSongPrefix))
                return false;
            SET_GPR_U32(ctx, 2, 1u);
            leave(ctx);
            return true;
        }
    }

    namespace
    {
        // CharDriver::Starved (0x1710b8): nothing queued after the clip it
        // plays (its first node, +0x38, with no next, +0x28), when a
        // guitarist's poll plays its idle group (GuitarCharacter::Poll,
        // 0x10ac90), whose song idles stand in their bind pose outside a
        // song. Never while a pre-show clip plays: the walk-on goes on to the
        // idle, and the idle loops. A node's clip is at +0x24, an Object
        // whose name is at +0x14 of its base.
        constexpr uint32_t kFirstNode = 0x38u, kNextNode = 0x28u, kNodeClip = 0x24u, kObjectName = 0x14u;
        struct StarvedTag;
        bool starved(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            const uint32_t node = load<uint32_t>(rdram, GPR_U32(ctx, 4) + kFirstNode);
            if (!node || load<uint32_t>(rdram, node + kNextNode))
                return false;
            const uint32_t clip = load<uint32_t>(rdram, node + kNodeClip);
            const uint32_t base = clip ? load<uint32_t>(rdram, clip) : 0u;
            const uint32_t name = base ? load<uint32_t>(rdram, base + kObjectName) : 0u;
            if (!name || !std::string_view(reinterpret_cast<const char *>(getMemPtr(rdram, name))).starts_with(kPreshow))
                return false;
            SET_GPR_U32(ctx, 2, 0u);
            leave(ctx);
            return true;
        }

        // {ghc_preshow_showing <driver>}: the clip its figure shows most of,
        // its node of most weight (+0x18): what is on screen, where
        // first_clip is the last played, which may not have started.
        constexpr uint32_t kNodeWeight = 0x18u;
        constexpr uint32_t kObjectNode = 0x4u; // DataNode's kDataObject
        constexpr uint32_t kDriverObject = 0x54u; // a CharDriver's Hmx::Object, which a script holds
        // The CharDriver a script passes, its first clip node, or 0.
        uint32_t firstNode(const script::Call &call)
        {
            const script::Node arg = call.arg(1);
            const uint32_t driver = arg.type == kObjectNode && arg.value ? arg.value - kDriverObject : 0u;
            return driver ? load<uint32_t>(call.rdram, driver + kFirstNode) : 0u;
        }

        script::Node showing(const script::Call &call)
        {
            uint32_t best = 0u;
            float most = -1.0f;
            for (uint32_t node = firstNode(call); node;
                 node = load<uint32_t>(call.rdram, node + kNextNode))
            {
                const float weight = load<float>(call.rdram, node + kNodeWeight);
                if (weight > most)
                    most = weight, best = node;
            }
            const uint32_t clip = best ? load<uint32_t>(call.rdram, best + kNodeClip) : 0u;
            const uint32_t base = clip ? load<uint32_t>(call.rdram, clip) : 0u;
            const uint32_t name = base ? load<uint32_t>(call.rdram, base + kObjectName) : 0u;
            const std::string text = name ? reinterpret_cast<const char *>(getMemPtr(call.rdram, name)) : "";
            return {script::symbol(call.rdram, call.ctx, call.runtime, text), script::kSymbol};
        }

        // {ghc_preshow_nodes <CharDriver>}: how many clips it holds, playing,
        // fading or queued.
        script::Node nodes(const script::Call &call)
        {
            int32_t count = 0;
            for (uint32_t node = firstNode(call); node;
                 node = load<uint32_t>(call.rdram, node + kNextNode))
                ++count;
            return {static_cast<uint32_t>(count), script::kInt};
        }

        // {ghc_sandbox_poll}: the file GHC_SANDBOX names, run as script each
        // time it is saved (tools/ghc.py sandbox).
        void sandboxPoll(const script::Call &call)
        {
            static const char *path = std::getenv("GHC_SANDBOX");
            static auto checked = std::chrono::steady_clock::time_point{};
            static std::filesystem::file_time_type seen{};
            if (!path || std::chrono::steady_clock::now() - checked < std::chrono::milliseconds(500))
                return;
            checked = std::chrono::steady_clock::now();
            std::error_code error;
            const auto saved = std::filesystem::last_write_time(path, error);
            if (error || saved == seen)
                return;
            seen = saved;
            std::ifstream in(path);
            const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            std::cerr << "[sandbox] " << path << " saved, run" << std::endl;
            script::run(call.rdram, call.ctx, call.runtime, "{do " + text + "\n}");
        }
    }

    void install(PS2Runtime &runtime, const Addresses &addresses)
    {
        Override<StarvedTag>::install(runtime, addresses.charDriverStarved, starved);
        script::addCommand("ghc_preshow_showing", showing);
        script::addCommand("ghc_preshow_nodes", nodes);
        script::addCommand("ghc_sandbox_poll", [](const script::Call &call) -> script::Node
                           {
                               sandboxPoll(call);
                               return {};
                           });
        const auto walkOns = [](const std::string &path)
        {
            return [path](milo::Dir &set)
            {
                if (!addWalkOns(set, "sd"))
                    std::cerr << "[menu_guitarist] no walk-ons in " << path << std::endl;
            };
        };
        for (const char *game : {"gh2", "gh80s"})
        {
            const auto disc = games::disc(game);
            if (!disc)
                continue;
            for (const std::string &main : ark::paths(*disc, "char/"))
            {
                if (!main.ends_with(kMain) || main.find("/anims/gen/") == std::string::npos)
                    continue;
                ark::addFix(*disc, main, [fix = walkOns(main)](std::vector<uint8_t> &file)
                            {
                                auto set = milo::read(file);
                                if (!set)
                                    return;
                                fix(*set);
                                file = milo::write(*set);
                            });
                if (std::string(game) == "gh2")
                    addGh2xSceneFix(main, walkOns(main));
            }
        }
        s_addresses = &addresses;
        EntryHook<FilePathSetTag>::install(runtime, addresses.filePathSet, onFilePathSet);
        Override<ValidCharTag>::install(runtime, addresses.charsysPanelValidChar, validChar);
        s_getClipGroup = runtime.lookupFunction(addresses.guitarCharacterGetClipGroup);
        runtime.replaceFunction(addresses.guitarCharacterGetClipGroup, &getClipGroup);
    }
}
