#include "save/gh1_stream.h"

#include "save/bin.h"

#include <cstring>

namespace gh2::save::gh1
{
    namespace
    {
        constexpr int32_t kOldestVersion = 5; // Campaign::Load (0x138c30) loads nothing older
        constexpr int32_t kStarsVersion = 6;  // CampaignItem::Load reads stars from here on
        constexpr size_t kNameSize = 8;       // HighScoreEntry's name, strncpy'd

        void readState(bin::Reader &r, ItemState &s, int32_t type, int32_t version)
        {
            s.flags = r.u8();
            if (type != kSongType)
                return;
            s.score = r.i32();
            if (version >= kStarsVersion)
                s.stars = r.u8();
        }
    }

    Save blank()
    {
        Save save;
        for (int p = 0; p < kProfiles; ++p)
        {
            Profile &profile = save.profiles[p];
            profile.name = "profile_" + std::to_string(p);
            // OptionData's defaults: version 1, no lefty, volumes 1.0, stereo.
            const int32_t version = 1;
            const float volume = 1.0f;
            uint8_t *o = profile.options.data();
            std::memcpy(o, &version, 4);
            for (int v = 0; v < 3; ++v)
                std::memcpy(o + 6 + 4 * v, &volume, 4);
            o[18] = 1;
        }
        return save;
    }

    std::optional<Save> parse(const uint8_t *data, size_t size)
    {
        bin::Reader r{data, size};
        Save save;
        save.version = r.i32();
        if (save.version < kOldestVersion || save.version > kStarsVersion)
            return std::nullopt;
        for (Profile &p : save.profiles)
        {
            p.empty = r.u8();
            p.name = r.str();
            p.tutorials = r.i32();
            p.difficulty = r.i32();
            for (Difficulty &d : p.difficulties)
            {
                d.character = r.str();
                d.guitar = r.str();
                d.skin = r.str();
                d.cash = r.i32();
            }
            if (const uint8_t *options = r.take(kOptionsSize))
                std::memcpy(p.options.data(), options, kOptionsSize);
        }
        for (uint32_t n = r.count(), i = 0; i < n && r.ok; ++i)
        {
            Item &item = save.items.emplace_back();
            item.name = r.str();
            item.type = r.i32();
            for (ItemState &s : item.states)
                readState(r, s, item.type, save.version);
        }
        save.scoreVersion = r.i32();
        for (uint32_t n = r.count(), i = 0; i < n && r.ok; ++i)
        {
            SongScores &song = save.scores.emplace_back();
            song.song = r.str();
            for (ScoreList &list : song.lists)
                for (ScoreEntry &e : list)
                {
                    e.name = r.name(kNameSize);
                    e.score = r.i32();
                }
        }
        if (!r.ok)
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
            for (const Difficulty &d : p.difficulties)
            {
                w.str(d.character);
                w.str(d.guitar);
                w.str(d.skin);
                w.i32(d.cash);
            }
            w.bytes(p.options.data(), p.options.size());
        }
        w.i32(static_cast<int32_t>(save.items.size()));
        for (const Item &item : save.items)
        {
            w.str(item.name);
            w.i32(item.type);
            for (const ItemState &s : item.states)
            {
                w.u8(s.flags);
                if (item.type != kSongType)
                    continue;
                w.i32(s.score);
                if (save.version >= kStarsVersion)
                    w.u8(s.stars);
            }
        }
        w.i32(save.scoreVersion);
        w.i32(static_cast<int32_t>(save.scores.size()));
        for (const SongScores &song : save.scores)
        {
            w.str(song.song);
            for (const ScoreList &list : song.lists)
                for (const ScoreEntry &e : list)
                {
                    w.name(e.name, kNameSize, kNameSize);
                    w.i32(e.score);
                }
        }
        return w.out;
    }
}
