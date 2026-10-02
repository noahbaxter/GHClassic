#include "save/gh2_stream.h"

#include <algorithm>
#include <cstring>

namespace gh2::save::gh2
{
    namespace
    {
        constexpr int32_t kVersion = 13;      // Campaign::Load loads nothing older
        constexpr int32_t kOptionsVersion = 4;
        constexpr size_t kNameSize = 20;      // HighScoreEntry's name
        constexpr uint32_t kMaxString = 256;
        constexpr uint32_t kMaxCount = 4096;

        struct Reader
        {
            const uint8_t *data;
            size_t size;
            size_t at = 0;
            bool ok = true;

            const uint8_t *take(size_t n)
            {
                if (!ok || size - at < n)
                {
                    ok = false;
                    return nullptr;
                }
                const uint8_t *p = data + at;
                at += n;
                return p;
            }
            uint8_t u8()
            {
                const uint8_t *p = take(1);
                return p ? *p : 0;
            }
            int32_t i32()
            {
                int32_t v = 0;
                if (const uint8_t *p = take(4))
                    std::memcpy(&v, p, 4);
                return v;
            }
            uint32_t count()
            {
                const int32_t n = i32();
                if (n < 0 || static_cast<uint32_t>(n) > kMaxCount)
                    ok = false;
                return ok ? static_cast<uint32_t>(n) : 0u;
            }
            std::string str()
            {
                const int32_t n = i32();
                if (n < 0 || static_cast<uint32_t>(n) > kMaxString)
                    ok = false;
                const uint8_t *p = ok ? take(static_cast<size_t>(n)) : nullptr;
                return p ? std::string(reinterpret_cast<const char *>(p), static_cast<size_t>(n)) : std::string();
            }
            ItemState state(int32_t type)
            {
                ItemState s;
                s.flags = u8();
                if (type == kSongType)
                {
                    s.score = i32();
                    s.stars = u8();
                    s.trickle = u8();
                }
                return s;
            }
            Item item(size_t states)
            {
                Item item;
                item.name = str();
                item.type = i32();
                for (size_t i = 0; i < states && ok; ++i)
                    item.states.push_back(state(item.type));
                return item;
            }
        };

        struct Writer
        {
            std::vector<uint8_t> out;

            void u8(uint8_t v) { out.push_back(v); }
            void i32(int32_t v)
            {
                uint8_t b[4];
                std::memcpy(b, &v, 4);
                out.insert(out.end(), b, b + 4);
            }
            void str(const std::string &s)
            {
                i32(static_cast<int32_t>(s.size()));
                out.insert(out.end(), s.begin(), s.end());
            }
            void state(const ItemState &s, int32_t type)
            {
                u8(s.flags);
                if (type == kSongType)
                {
                    i32(s.score);
                    u8(s.stars);
                    u8(s.trickle);
                }
            }
            void item(const Item &item)
            {
                str(item.name);
                i32(item.type);
                for (const ItemState &s : item.states)
                    state(s, item.type);
            }
        };
    }

    std::optional<Save> parse(const uint8_t *data, size_t size)
    {
        Reader r{data, size};
        Save save;
        save.version = r.i32();
        if (save.version != kVersion)
            return std::nullopt;
        for (Profile &p : save.profiles)
        {
            p.empty = r.u8();
            p.name = r.str();
            p.tutorials = r.i32();
            p.difficulty = r.i32();
            p.cash = r.i32();
            for (auto &look : p.look)
                for (std::string &part : look)
                    part = r.str();
        }
        for (uint32_t n = r.count(), i = 0; i < n && r.ok; ++i)
            save.profileItems.push_back(r.item(kProfiles));
        for (uint32_t n = r.count(), i = 0; i < n && r.ok; ++i)
            save.campaignItems.push_back(r.item(kProfiles * kDifficulties));
        save.scoreVersion = r.i32();
        for (uint32_t n = r.count(), i = 0; i < n && r.ok; ++i)
        {
            SongScores &song = save.scores.emplace_back();
            song.song = r.str();
            for (auto &list : song.lists)
                for (ScoreEntry &e : list)
                {
                    const uint8_t *name = r.take(kNameSize);
                    if (name)
                        e.name.assign(name, std::find(name, name + kNameSize, uint8_t{0}));
                    e.score = r.i32();
                }
        }
        save.defaultName = r.str();
        save.careerBeaten = r.u8();
        for (uint32_t n = r.count(), i = 0; i < n && r.ok; ++i)
            save.coop.push_back(r.state(kSongType));
        if (const uint8_t *options = r.take(kOptionsSize))
            std::memcpy(save.options.data(), options, kOptionsSize);
        int32_t optionsVersion = 0;
        std::memcpy(&optionsVersion, save.options.data(), 4);
        if (!r.ok || optionsVersion != kOptionsVersion)
            return std::nullopt;
        return save;
    }

    std::vector<uint8_t> write(const Save &save)
    {
        Writer w;
        w.i32(save.version);
        for (const Profile &p : save.profiles)
        {
            w.u8(p.empty);
            w.str(p.name);
            w.i32(p.tutorials);
            w.i32(p.difficulty);
            w.i32(p.cash);
            for (const auto &look : p.look)
                for (const std::string &part : look)
                    w.str(part);
        }
        w.i32(static_cast<int32_t>(save.profileItems.size()));
        for (const Item &item : save.profileItems)
            w.item(item);
        w.i32(static_cast<int32_t>(save.campaignItems.size()));
        for (const Item &item : save.campaignItems)
            w.item(item);
        w.i32(save.scoreVersion);
        w.i32(static_cast<int32_t>(save.scores.size()));
        for (const SongScores &song : save.scores)
        {
            w.str(song.song);
            for (const auto &list : song.lists)
                for (const ScoreEntry &e : list)
                {
                    char name[kNameSize] = {};
                    std::memcpy(name, e.name.data(), std::min(e.name.size(), kNameSize - 1));
                    w.out.insert(w.out.end(), name, name + kNameSize);
                    w.i32(e.score);
                }
        }
        w.str(save.defaultName);
        w.u8(save.careerBeaten);
        w.i32(static_cast<int32_t>(save.coop.size()));
        for (const ItemState &s : save.coop)
            w.state(s, kSongType);
        w.out.insert(w.out.end(), save.options.begin(), save.options.end());
        return w.out;
    }
}
