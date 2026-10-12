#include "save/gh2_stream.h"

#include "save/bin.h"

#include <cstring>

namespace gh2::save::gh2
{
    namespace
    {
        constexpr int32_t kVersion = 13;      // Campaign::Load loads nothing older
        constexpr int32_t kOptionsVersion = 4;
        constexpr size_t kNameSize = 20;      // HighScoreEntry's name

        ItemState readState(bin::Reader &r, int32_t type)
        {
            ItemState s;
            s.flags = r.u8();
            if (type == kSongType)
            {
                s.score = r.i32();
                s.stars = r.u8();
                s.trickle = r.u8();
            }
            return s;
        }

        Item readItem(bin::Reader &r, size_t states)
        {
            Item item;
            item.name = r.str();
            item.type = r.i32();
            for (size_t i = 0; i < states && r.ok; ++i)
                item.states.push_back(readState(r, item.type));
            return item;
        }

        void writeState(bin::Writer &w, const ItemState &s, int32_t type)
        {
            w.u8(s.flags);
            if (type == kSongType)
            {
                w.i32(s.score);
                w.u8(s.stars);
                w.u8(s.trickle);
            }
        }

        void writeItem(bin::Writer &w, const Item &item)
        {
            w.str(item.name);
            w.i32(item.type);
            for (const ItemState &s : item.states)
                writeState(w, s, item.type);
        }
    }

    Save blank()
    {
        Save save;
        save.version = kVersion;
        save.scoreVersion = 3;
        for (int p = 0; p < kProfiles; ++p)
            save.profiles[p].name = "profile_" + std::to_string(p);
        // Version, lefty for each player, three volumes at 11, stereo on.
        const int32_t fields[] = {kOptionsVersion, 0, 11, 11, 11};
        uint8_t *o = save.options.data();
        std::memcpy(o, &fields[0], 4);
        for (int v = 0; v < 3; ++v)
            std::memcpy(o + 6 + 4 * v, &fields[2 + v], 4);
        o[18] = 1;
        return save;
    }

    std::optional<Save> parse(const uint8_t *data, size_t size)
    {
        bin::Reader r{data, size};
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
            save.profileItems.push_back(readItem(r, kProfiles));
        for (uint32_t n = r.count(), i = 0; i < n && r.ok; ++i)
            save.campaignItems.push_back(readItem(r, kProfiles * kDifficulties));
        save.scoreVersion = r.i32();
        for (uint32_t n = r.count(), i = 0; i < n && r.ok; ++i)
        {
            SongScores &song = save.scores.emplace_back();
            song.song = r.str();
            for (auto &list : song.lists)
                for (ScoreEntry &e : list)
                {
                    e.name = r.name(kNameSize);
                    e.score = r.i32();
                }
        }
        save.defaultName = r.str();
        save.careerBeaten = r.u8();
        for (uint32_t n = r.count(), i = 0; i < n && r.ok; ++i)
            save.coop.push_back(readState(r, kSongType));
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
        bin::Writer w;
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
            writeItem(w, item);
        w.i32(static_cast<int32_t>(save.campaignItems.size()));
        for (const Item &item : save.campaignItems)
            writeItem(w, item);
        w.i32(save.scoreVersion);
        w.i32(static_cast<int32_t>(save.scores.size()));
        for (const SongScores &song : save.scores)
        {
            w.str(song.song);
            for (const auto &list : song.lists)
                for (const ScoreEntry &e : list)
                {
                    w.name(e.name, kNameSize, kNameSize - 1);
                    w.i32(e.score);
                }
        }
        w.str(save.defaultName);
        w.u8(save.careerBeaten);
        w.i32(static_cast<int32_t>(save.coop.size()));
        for (const ItemState &s : save.coop)
            writeState(w, s, kSongType);
        w.bytes(save.options.data(), save.options.size());
        return w.out;
    }
}
