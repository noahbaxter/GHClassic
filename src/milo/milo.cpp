// The block wrapper (header: magic, first block's offset, block count, the
// largest block inflated, each block's stored size) and the directory inside:
// v10 (GH1) is entries then an externals list; v24 (GH2) names its root
// class and keeps that object's data ahead of the entries', and v25 (360
// GH2) is v24's with other class revisions, read only. Each body ends
// with ADDEADDE. Layouts from Mackiloha, grim and MiloEditor, checked
// against retail files; the block rules (whole objects, the inflated size in
// the header) are what the game's ChunkStream needs.

#include "milo/milo.h"

#include <miniz.h>

#include <algorithm>
#include <cstring>
#include <map>
#include <set>

namespace gh2::milo
{
    namespace
    {
        constexpr uint32_t kDeflated = 0xCBBEDEAFu;
        constexpr uint32_t kStored = 0xCABEDEAFu;
        constexpr uint32_t kDeflatedSized = 0xCDBEDEAFu; // the 360's: each block's inflated size first
        constexpr uint8_t kMark[4] = {0xad, 0xde, 0xad, 0xde};
        // Retail closes a block at the first object boundary past this.
        constexpr size_t kBlockSize = 0x20000u;

        // The revisions each game writes for the classes in an outfit. A
        // body can hold the marker bytes, so a marker only ends one when the
        // next entry's revision follows it. A grafted v24 file holds GH1's
        // beside GH2's.
        const std::map<std::string, std::set<uint32_t>> kGh1Revs = {
            {"Tex", {8}}, {"Mat", {21}}, {"Mesh", {25}}, {"View", {7}}};
        const std::map<std::string, std::set<uint32_t>> kGh2Revs = {
            {"Tex", {8, 10}}, {"Mat", {21, 27}}, {"Mesh", {25, 28}}, {"Group", {11, 12}}, {"Trans", {9}}}; // Group 11: the GH2 demo's
        // Every class in the 360 disc's v25 files, at the one revision it
        // writes each.
        const std::map<std::string, std::set<uint32_t>> kXboxRevs = {
            {"AnimFilter", {1}}, {"BandButton", {10}}, {"BandCharacter", {1}}, {"BandCrowdMeterDir", {3}},
            {"BandLabel", {12}}, {"BandLeadMeter", {8}}, {"BandPlacer", {2}}, {"BandScoreDisplay", {3}},
            {"BandSlider", {2}}, {"BandStarMeterDir", {5}}, {"BandStreakDisplay", {4}}, {"BandTextEntry", {3}},
            {"Cam", {12}}, {"CamAnim", {2}}, {"CamShot", {27}}, {"CharBone", {3}}, {"CharClipFilter", {1}},
            {"CharClipGroup", {1}}, {"CharClipSamples", {11}}, {"CharDriver", {5}}, {"CharDriverMidi", {3}},
            {"CharEyes", {4}}, {"CharForeTwist", {1}}, {"CharHair", {2}}, {"CharIKHand", {5}}, {"CharIKMidi", {4}},
            {"CharIKRod", {2}}, {"CharLookAt", {3}}, {"CharPosConstraint", {2}}, {"CharServoBone", {1}},
            {"CharUpperTwist", {1}}, {"CharWalk", {1}}, {"CharWeightSetter", {3}}, {"Character", {10}},
            {"CheckBox", {0}}, {"CubeTex", {1}}, {"EnvAnim", {4}}, {"Environ", {5}}, {"EventTrigger", {10}},
            {"FaceFxLipSyncServo", {5}}, {"Flare", {4}}, {"Font", {15}}, {"FxSendReverb", {1}}, {"GHTrackDir", {3}},
            {"Group", {12}}, {"Light", {9}}, {"LightAnim", {2}}, {"LightPreset", {9}}, {"Mat", {28}},
            {"MatAnim", {7}}, {"Mesh", {28}}, {"MeshAnim", {1}}, {"ObjectDir", {17}}, {"OutfitLoader", {2}},
            {"ParticleSys", {30}}, {"ParticleSysAnim", {3}}, {"PostProc", {9}}, {"RandomGroupSeq", {1}},
            {"RndDir", {9}}, {"ScreenMask", {2}}, {"Screenshot", {1}}, {"SerialGroupSeq", {1}}, {"Set", {0}},
            {"Sfx", {5}}, {"SfxSeq", {3}}, {"Spotlight", {25}}, {"SynthSample", {5}}, {"Tex", {10}}, {"Text", {17}},
            {"TrackWidget", {4}}, {"Trans", {9}}, {"TransAnim", {6}}, {"UILabel", {1}}, {"UIList", {3}},
            {"UIPicture", {0}}, {"UIProxy", {0}}, {"UITrigger", {0}}, {"WaitSeq", {2}}, {"Waypoint", {3}},
            {"WorldCrowd", {6}}};

        struct Reader
        {
            const Bytes &b;
            size_t o = 0;
            bool ok = true;

            uint32_t u32()
            {
                if (o + 4u > b.size())
                {
                    ok = false;
                    return 0u;
                }
                uint32_t v;
                std::memcpy(&v, b.data() + o, 4u);
                o += 4u;
                return v;
            }

            std::string str()
            {
                const uint32_t n = u32();
                if (!ok || o + n > b.size())
                {
                    ok = false;
                    return {};
                }
                std::string s(reinterpret_cast<const char *>(b.data() + o), n);
                o += n;
                return s;
            }
        };

        constexpr size_t kNone = static_cast<size_t>(-1);

        size_t findMark(const Bytes &b, size_t from)
        {
            const auto it = std::search(b.begin() + static_cast<std::ptrdiff_t>(from), b.end(), kMark, kMark + 4);
            return it == b.end() ? kNone : static_cast<size_t>(it - b.begin());
        }
    }

    uint32_t u32(const Bytes &b, size_t o)
    {
        uint32_t v = 0u;
        if (o + 4u <= b.size())
            std::memcpy(&v, b.data() + o, 4u);
        return v;
    }

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

    std::optional<Bytes> inflate(const Bytes &file)
    {
        Reader r{file};
        const uint32_t magic = r.u32();
        const uint32_t first = r.u32();
        const uint32_t count = r.u32();
        const uint32_t largest = r.u32();
        if (!r.ok || (magic != kDeflated && magic != kStored && magic != kDeflatedSized))
            return std::nullopt;
        const size_t skip = magic == kDeflatedSized ? 4u : 0u;
        Bytes out;
        Bytes block(largest);
        size_t at = first;
        for (uint32_t i = 0; i < count; ++i)
        {
            const uint32_t size = r.u32() & 0xFFFFFFu;
            if (!r.ok || at + size > file.size() || size < skip)
                return std::nullopt;
            if (magic == kStored)
                out.insert(out.end(), file.begin() + static_cast<std::ptrdiff_t>(at),
                           file.begin() + static_cast<std::ptrdiff_t>(at + size));
            else
            {
                const size_t n = tinfl_decompress_mem_to_mem(block.data(), block.size(), file.data() + at + skip,
                                                             size - skip, 0);
                if (n == TINFL_DECOMPRESS_MEM_TO_MEM_FAILED)
                    return std::nullopt;
                out.insert(out.end(), block.begin(), block.begin() + static_cast<std::ptrdiff_t>(n));
            }
            at += size;
        }
        return out;
    }

    std::optional<Dir> parse(const Bytes &raw)
    {
        Reader r{raw};
        Dir dir;
        dir.version = r.u32();
        if (dir.version != 10u && dir.version != 24u && dir.version != 25u)
            return std::nullopt;
        const bool rooted = dir.version != 10u;
        if (rooted)
        {
            dir.className = r.str();
            dir.name = r.str();
            dir.tableCount = r.u32();
            dir.tableSize = r.u32();
        }
        const uint32_t count = r.u32();
        for (uint32_t i = 0; i < count && r.ok; ++i)
        {
            std::string c = r.str();
            std::string n = r.str();
            dir.entries.emplace_back(std::move(c), std::move(n));
        }
        if (dir.version == 10u)
        {
            const uint32_t externals = r.u32();
            for (uint32_t i = 0; i < externals && r.ok; ++i)
                r.str();
        }
        if (!r.ok)
            return std::nullopt;

        // The root (v24 and v25), then each entry, in order.
        const auto &revs = dir.version == 10u ? kGh1Revs : dir.version == 24u ? kGh2Revs : kXboxRevs;
        std::vector<std::string> order;
        if (rooted)
            order.push_back("");
        for (const auto &e : dir.entries)
            order.push_back(e.first);
        size_t start = r.o;
        size_t search = start;
        for (size_t i = 0; i < order.size(); ++i)
        {
            const std::string *next = i + 1 < order.size() ? &order[i + 1] : nullptr;
            for (;;)
            {
                const size_t mark = findMark(raw, search);
                if (mark == kNone)
                    return std::nullopt;
                const size_t after = mark + 4u;
                bool ends = next == nullptr && after == raw.size();
                if (next && after + 4u <= raw.size())
                {
                    uint32_t rev;
                    std::memcpy(&rev, raw.data() + after, 4u);
                    const auto it = revs.find(*next);
                    ends = it != revs.end() ? it->second.count(rev) != 0u : rev < 0x100u;
                }
                if (ends)
                {
                    Bytes body(raw.begin() + static_cast<std::ptrdiff_t>(start),
                               raw.begin() + static_cast<std::ptrdiff_t>(mark));
                    if (rooted && i == 0)
                        dir.root = std::move(body);
                    else
                        dir.bodies.push_back(std::move(body));
                    start = search = after;
                    break;
                }
                search = mark + 1u;
            }
        }
        if (start != raw.size())
            return std::nullopt;
        return dir;
    }

    void add(Dir &dir, const std::string &className, const std::string &name, Bytes body)
    {
        dir.tableCount += 2u;
        dir.tableSize += static_cast<uint32_t>(className.size() + name.size() + 2u);
        dir.entries.emplace_back(className, name);
        dir.bodies.push_back(std::move(body));
    }

    void replacePrefix(Dir &dir, const std::string &from, const std::string &to)
    {
        auto replace = [&](Bytes &b)
        {
            Bytes out;
            size_t o = 0u;
            while (o < b.size())
            {
                uint32_t n = 0u;
                if (o + 4u <= b.size())
                    std::memcpy(&n, b.data() + o, 4u);
                const auto *s = b.data() + o + 4u;
                if (n >= from.size() && o + 4u + n <= b.size() && std::memcmp(s, from.data(), from.size()) == 0)
                {
                    putStr(out, to + std::string(reinterpret_cast<const char *>(s) + from.size(), n - from.size()));
                    o += 4u + n;
                }
                else
                    out.push_back(b[o++]);
            }
            b = std::move(out);
        };
        replace(dir.root);
        for (Bytes &b : dir.bodies)
            replace(b);
    }

    std::optional<std::string> findSuffix(const Dir &dir, const std::string &suffix)
    {
        auto find = [&](const Bytes &b) -> std::optional<std::string>
        {
            for (size_t o = 0u; o + 4u <= b.size(); ++o)
            {
                uint32_t n;
                std::memcpy(&n, b.data() + o, 4u);
                if (n < suffix.size() || n > 256u || o + 4u + n > b.size())
                    continue;
                const std::string s(reinterpret_cast<const char *>(b.data() + o + 4u), n);
                if (s.compare(n - suffix.size(), suffix.size(), suffix) == 0)
                    return s;
            }
            return std::nullopt;
        };
        if (auto s = find(dir.root))
            return s;
        for (const Bytes &b : dir.bodies)
            if (auto s = find(b))
                return s;
        return std::nullopt;
    }

    namespace
    {
        Bytes lengthPrefixed(const std::string &s)
        {
            Bytes out;
            putStr(out, s);
            return out;
        }

        // `name` put in the list holding `member`, after it or before it.
        bool addBeside(Bytes &body, const std::string &member, const std::string &name, bool after)
        {
            const auto count = listHolding(body, member);
            if (!count)
                return false;
            const Bytes there = lengthPrefixed(member), added = lengthPrefixed(name);
            const auto at = std::search(body.begin(), body.end(), there.begin(), there.end());
            body.insert(after ? at + static_cast<std::ptrdiff_t>(there.size()) : at, added.begin(), added.end());
            const uint32_t n = u32(body, *count) + 1u;
            std::memcpy(body.data() + *count, &n, 4u);
            return true;
        }
    }

    bool replaceStr(Bytes &body, const std::string &from, const std::string &to)
    {
        const Bytes a = lengthPrefixed(from), b = lengthPrefixed(to);
        const auto it = std::search(body.begin(), body.end(), a.begin(), a.end());
        if (it == body.end())
            return false;
        const auto at = body.erase(it, it + static_cast<std::ptrdiff_t>(a.size()));
        body.insert(at, b.begin(), b.end());
        return true;
    }

    std::optional<size_t> listHolding(const Bytes &body, const std::string &name)
    {
        const Bytes member = lengthPrefixed(name);
        const auto it = std::search(body.begin(), body.end(), member.begin(), member.end());
        if (it == body.end())
            return std::nullopt;
        const size_t at = static_cast<size_t>(it - body.begin());
        for (size_t count = at; count >= 4u; --count)
        {
            const uint32_t n = u32(body, count - 4u);
            if (n == 0u || n > 64u)
                continue;
            size_t o = count;
            bool covers = false;
            for (uint32_t k = 0; k < n && o < body.size(); ++k)
            {
                covers = covers || o == at;
                str(body, o);
            }
            if (covers)
                return count - 4u;
        }
        return std::nullopt;
    }

    bool addAfter(Bytes &body, const std::string &after, const std::string &name)
    {
        return addBeside(body, after, name, true);
    }

    bool addBefore(Bytes &body, const std::string &before, const std::string &name)
    {
        return addBeside(body, before, name, false);
    }

    bool remove(Bytes &body, const std::string &name)
    {
        const auto count = listHolding(body, name);
        if (!count)
            return false;
        const Bytes member = lengthPrefixed(name);
        const auto at = std::search(body.begin(), body.end(), member.begin(), member.end());
        body.erase(at, at + static_cast<std::ptrdiff_t>(member.size()));
        const uint32_t n = u32(body, *count) - 1u;
        std::memcpy(body.data() + *count, &n, 4u);
        return true;
    }

    std::optional<Dir> read(const Bytes &file)
    {
        const auto raw = inflate(file);
        return parse(raw ? *raw : file);
    }

    std::optional<size_t> find(const Dir &dir, const std::string &name)
    {
        for (size_t i = 0; i < dir.entries.size(); ++i)
            if (dir.entries[i].second == name)
                return i;
        return std::nullopt;
    }

    Bytes write(const Dir &dir)
    {
        Bytes raw;
        putU32(raw, 24u);
        putStr(raw, dir.className);
        putStr(raw, dir.name);
        putU32(raw, dir.tableCount);
        putU32(raw, dir.tableSize);
        putU32(raw, static_cast<uint32_t>(dir.entries.size()));
        for (const auto &[c, n] : dir.entries)
        {
            putStr(raw, c);
            putStr(raw, n);
        }
        std::vector<size_t> ends;
        auto body = [&](const Bytes &b)
        {
            raw.insert(raw.end(), b.begin(), b.end());
            raw.insert(raw.end(), kMark, kMark + 4);
            ends.push_back(raw.size());
        };
        body(dir.root);
        for (const Bytes &b : dir.bodies)
            body(b);

        std::vector<std::pair<size_t, size_t>> blocks; // offset, size
        size_t blockStart = 0u;
        for (const size_t end : ends)
        {
            if (end - blockStart >= kBlockSize || end == raw.size())
            {
                blocks.emplace_back(blockStart, end - blockStart);
                blockStart = end;
            }
        }
        size_t largest = 0u;
        for (const auto &b : blocks)
            largest = std::max(largest, b.second);

        Bytes file;
        const uint32_t first = std::max<uint32_t>(0x210u, 16u + 4u * static_cast<uint32_t>(blocks.size()));
        putU32(file, kStored);
        putU32(file, first);
        putU32(file, static_cast<uint32_t>(blocks.size()));
        putU32(file, static_cast<uint32_t>(largest));
        for (const auto &b : blocks)
            putU32(file, static_cast<uint32_t>(b.second));
        file.resize(first, 0u);
        file.insert(file.end(), raw.begin(), raw.end());
        return file;
    }
}
