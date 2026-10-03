#include "save/gh2_text.h"

#include <charconv>
#include <iostream>
#include <map>
#include <optional>
#include <sstream>

namespace gh2::save::gh2
{
    namespace
    {
        const char *const kDifficultyNames[kDifficulties] = {"easy", "medium", "hard", "expert"};
        // CampaignItem types as the save holds them: venues and songs per
        // difficulty, the rest per band. Outfits include the characters.
        const char *const kTypes[] = {"venue", "song", "outfit", "guitar", "finish", "video", "mode"};
        const char *const kLook[4] = {"character", "outfit", "guitar", "finish"};
        const char *const kFlags[3] = {"store", "unlocked", "passed"};
        constexpr uint8_t kGold = 0x80u;

        std::string band(int profile)
        {
            return "band " + std::to_string(profile + 1);
        }

        std::string typeName(int32_t type)
        {
            return type >= 0 && type < static_cast<int32_t>(std::size(kTypes)) ? kTypes[type]
                                                                               : "type" + std::to_string(type);
        }

        std::optional<int32_t> number(const std::string &text)
        {
            int32_t v = 0;
            const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), v);
            if (error != std::errc() || end != text.data() + text.size())
                return std::nullopt;
            return v;
        }

        std::optional<int32_t> difficulty(const std::string &name)
        {
            for (int d = 0; d < kDifficulties; ++d)
                if (name == kDifficultyNames[d])
                    return d;
            return std::nullopt;
        }

        // "<type>.<name>", with "#2" on for an item listed twice: the
        // characters share their outfits' names.
        std::vector<std::string> itemKeys(const std::vector<Item> &items)
        {
            std::map<std::string, int> seen;
            std::vector<std::string> keys;
            for (const Item &item : items)
            {
                std::string key = typeName(item.type) + "." + item.name;
                if (const int n = ++seen[key]; n > 1)
                    key += "#" + std::to_string(n);
                keys.push_back(key);
            }
            return keys;
        }

        std::string formatState(const ItemState &s, int32_t type)
        {
            std::string out;
            for (int bit = 0; bit < 3; ++bit)
                if (s.flags & (1u << bit))
                    out += std::string(out.empty() ? "" : " ") + kFlags[bit];
            if (out.empty())
                out = "locked";
            if (type != kSongType)
                return out;
            if (s.score)
                out += " score=" + std::to_string(s.score);
            if (s.stars & ~kGold)
                out += " stars=" + std::to_string(s.stars & ~kGold);
            if (s.stars & kGold)
                out += " gold";
            if (s.trickle)
                out += " trickle=" + std::to_string(s.trickle);
            return out;
        }

        std::optional<ItemState> parseState(const std::string &text, int32_t type)
        {
            ItemState s;
            std::istringstream in(text);
            std::string word;
            while (in >> word)
            {
                const size_t eq = word.find('=');
                const std::string name = word.substr(0, eq);
                const std::optional<int32_t> value =
                    eq == std::string::npos ? std::nullopt : number(word.substr(eq + 1));
                bool known = false;
                for (int bit = 0; bit < 3; ++bit)
                    if (word == kFlags[bit])
                    {
                        s.flags |= static_cast<uint8_t>(1u << bit);
                        known = true;
                    }
                if (word == "locked")
                    known = true;
                else if (type == kSongType && word == "gold")
                {
                    s.stars |= kGold;
                    known = true;
                }
                else if (type == kSongType && value && name == "score")
                {
                    s.score = *value;
                    known = true;
                }
                else if (type == kSongType && value && name == "stars" && *value >= 0 && *value < kGold)
                {
                    s.stars = static_cast<uint8_t>((s.stars & kGold) | *value);
                    known = true;
                }
                else if (type == kSongType && value && name == "trickle" && *value >= 0 && *value <= 0xff)
                {
                    s.trickle = static_cast<uint8_t>(*value);
                    known = true;
                }
                if (!known)
                    return std::nullopt;
            }
            return s;
        }

        // The items' states at index that differ from fresh's.
        void writeItems(Section &section, const std::vector<Item> &items, const std::vector<Item> &fresh,
                        size_t index)
        {
            const std::vector<std::string> keys = itemKeys(items);
            const std::vector<std::string> freshKeys = itemKeys(fresh);
            std::map<std::string, const Item *> byKey;
            for (size_t i = 0; i < fresh.size(); ++i)
                byKey[freshKeys[i]] = &fresh[i];
            for (size_t i = 0; i < items.size(); ++i)
            {
                const auto it = byKey.find(keys[i]);
                const ItemState &state = items[i].states[index];
                if (it != byKey.end() && it->second->type == items[i].type && it->second->states[index] == state)
                    continue;
                section.set(keys[i], formatState(state, items[i].type));
            }
        }

        bool readItem(std::vector<Item> &items, const std::vector<std::string> &keys, size_t index,
                      const std::string &key, const std::string &value)
        {
            for (size_t i = 0; i < items.size(); ++i)
            {
                if (keys[i] != key)
                    continue;
                const std::optional<ItemState> state = parseState(value, items[i].type);
                if (!state)
                    return false;
                items[i].states[index] = *state;
                return true;
            }
            return false;
        }

        // The coop list has no names: expert's career songs, then the store's,
        // the same songs in the same order as the campaign's song items.
        std::vector<std::string> coopKeys(const Save &fresh)
        {
            std::vector<std::string> keys;
            for (const Item &item : fresh.campaignItems)
                if (item.type == kSongType)
                    keys.push_back("song." + item.name);
            if (keys.size() != fresh.coop.size())
            {
                keys.clear();
                for (size_t i = 0; i < fresh.coop.size(); ++i)
                    keys.push_back("slot." + std::to_string(i));
            }
            return keys;
        }

        std::string gameOf(const Games &games, const std::string &song)
        {
            const auto it = games.songs.find(song);
            return it == games.songs.end() ? games.own : it->second;
        }

        // "<game> scores <difficulty>", for the career's game or one whose
        // songs it holds.
        std::optional<std::pair<std::string, int32_t>> scoreSection(const Games &games, const std::string &name)
        {
            const size_t at = name.find(" scores ");
            if (at == std::string::npos)
                return std::nullopt;
            const std::string game = name.substr(0, at);
            const auto d = difficulty(name.substr(at + 8));
            bool known = game == games.own;
            for (const auto &[song, g] : games.songs)
                known = known || g == game;
            if (!known || !d)
                return std::nullopt;
            return std::pair{game, *d};
        }

        // A band's own section, or one of its sections for any game.
        bool ofBand(const std::string &name, int profile)
        {
            const std::string b = band(profile);
            return name == b || name.rfind(b + " ", 0) == 0;
        }

        bool owned(const Games &games, const std::string &name)
        {
            if (name == games.own || name == games.own + " coop" || scoreSection(games, name))
                return true;
            for (int p = 0; p < kProfiles; ++p)
            {
                const std::string b = band(p), mine = b + " " + games.own;
                if (name == b || name == mine || name.rfind(mine + " ", 0) == 0)
                    return true;
            }
            return false;
        }

        // A fresh campaign's keys, which say what a store's keys address.
        struct Keys
        {
            std::vector<std::string> profile, campaign, coop;
            const Games &games;

            Keys(const Save &fresh, const Games &songGames)
                : profile(itemKeys(fresh.profileItems)), campaign(itemKeys(fresh.campaignItems)), coop(coopKeys(fresh)),
                  games(songGames)
            {
            }
        };

        // Lays one key over save; false when this build has nothing it addresses.
        bool apply(Save &save, const Keys &keys, const std::string &section, const std::string &key,
                   const std::string &value)
        {
            const std::string &own = keys.games.own;
            if (section == own)
            {
                const auto v = number(value);
                // Each game kept its own once; [classic]'s is read after.
                if (key == "default_name")
                    save.defaultName = value;
                else if (key == "career_beaten" && v)
                    save.careerBeaten = static_cast<uint8_t>(*v);
                else
                    return false;
                return true;
            }
            if (section == own + " coop")
            {
                for (size_t i = 0; i < keys.coop.size(); ++i)
                    if (keys.coop[i] == key)
                        if (const auto state = parseState(value, kSongType))
                        {
                            save.coop[i] = *state;
                            return true;
                        }
                return false;
            }
            if (const auto scores = scoreSection(keys.games, section))
            {
                const auto &[game, d] = *scores;
                const size_t dot = key.rfind('.');
                const auto rank = dot == std::string::npos ? std::nullopt : number(key.substr(dot + 1));
                const size_t space = value.find(' ');
                const auto score = number(value.substr(0, space));
                if (!rank || *rank < 1 || *rank > kScoresPerList || !score)
                    return false;
                for (SongScores &song : save.scores)
                    if (song.song == key.substr(0, dot) && gameOf(keys.games, song.song) == game)
                    {
                        song.lists[d][*rank - 1] = {space == std::string::npos ? "" : value.substr(space + 1), *score};
                        return true;
                    }
                return false;
            }
            for (int p = 0; p < kProfiles; ++p)
            {
                Profile &profile = save.profiles[p];
                const std::string b = band(p);
                if (section == b)
                {
                    if (key != "name")
                        return false;
                    profile.empty = 0;
                    profile.name = value;
                    return true;
                }
                const std::string mine = b + " " + own;
                if (section == mine)
                {
                    const auto v = number(value);
                    if (key == "tutorials" && v)
                        profile.tutorials = *v;
                    else if (key == "cash" && v)
                        profile.cash = *v;
                    else if (const auto d = difficulty(value); key == "difficulty" && d)
                        profile.difficulty = *d;
                    else
                        return readItem(save.profileItems, keys.profile, p, key, value);
                    return true;
                }
                if (section.rfind(mine + " ", 0) == 0)
                {
                    const auto d = difficulty(section.substr(mine.size() + 1));
                    if (!d)
                        return false;
                    for (int part = 0; part < 4; ++part)
                        if (key == kLook[part])
                        {
                            profile.look[*d][part] = value;
                            return true;
                        }
                    return readItem(save.campaignItems, keys.campaign, p * kDifficulties + *d, key, value);
                }
            }
            return false;
        }
    }

    void toStore(const Save &save, const Save &fresh, Store &store, const Games &games)
    {
        store.removeIf([&](const std::string &name)
                       {
                           for (int p = 0; p < kProfiles; ++p)
                               if (save.profiles[p].empty && ofBand(name, p))
                                   return true;
                           return false;
                       });
        // What this build cannot address stays for one that can: an outfit
        // another build adds keeps its progress through a session without it.
        const Keys keys(fresh, games);
        const auto ours = [&](const std::string &name) { return owned(games, name); };
        std::vector<std::pair<std::string, std::pair<std::string, std::string>>> kept;
        Save scratch = fresh;
        for (const Section &section : store.sections)
            if (ours(section.name))
                for (const auto &[key, value] : section.values)
                    if (!apply(scratch, keys, section.name, key, value))
                        kept.push_back({section.name, {key, value}});
        store.removeIf(ours);
        Store out;

        if (save.defaultName != fresh.defaultName)
            store.section(kOwnSection).set("default_name", save.defaultName);
        else if (store.find(kOwnSection))
            std::erase_if(store.section(kOwnSection).values,
                          [](const auto &entry) { return entry.first == "default_name"; });

        Section &game = out.section(games.own);
        if (save.careerBeaten != fresh.careerBeaten)
            game.set("career_beaten", std::to_string(save.careerBeaten));

        for (int p = 0; p < kProfiles; ++p)
        {
            const Profile &profile = save.profiles[p];
            const Profile &base = fresh.profiles[p];
            if (!profile.empty)
                out.section(band(p)).set("name", profile.name);

            Section &own = out.section(band(p) + " " + games.own);
            if (profile.tutorials != base.tutorials)
                own.set("tutorials", std::to_string(profile.tutorials));
            if (profile.difficulty != base.difficulty && profile.difficulty >= 0 && profile.difficulty < kDifficulties)
                own.set("difficulty", kDifficultyNames[profile.difficulty]);
            if (profile.cash != base.cash)
                own.set("cash", std::to_string(profile.cash));
            writeItems(own, save.profileItems, fresh.profileItems, p);

            for (int d = 0; d < kDifficulties; ++d)
            {
                Section &diff = out.section(band(p) + " " + games.own + " " + kDifficultyNames[d]);
                for (int part = 0; part < 4; ++part)
                    if (profile.look[d][part] != base.look[d][part])
                        diff.set(kLook[part], profile.look[d][part]);
                writeItems(diff, save.campaignItems, fresh.campaignItems, p * kDifficulties + d);
            }
        }

        Section &coop = out.section(games.own + " coop");
        for (size_t i = 0; i < save.coop.size() && i < keys.coop.size(); ++i)
            if (save.coop[i] != fresh.coop[i])
                coop.set(keys.coop[i], formatState(save.coop[i], kSongType));

        // Every song of a game starts on the same five entries.
        for (int d = 0; d < kDifficulties; ++d)
        {
            for (const SongScores &song : save.scores)
            {
                Section &scores = out.section(gameOf(games, song.song) + " scores " + kDifficultyNames[d]);
                const SongScores *base = nullptr;
                for (const SongScores &s : fresh.scores)
                    if (s.song == song.song)
                        base = &s;
                for (int rank = 0; rank < kScoresPerList; ++rank)
                {
                    const ScoreEntry &e = song.lists[d][rank];
                    if (base && base->lists[d][rank] == e)
                        continue;
                    scores.set(song.song + "." + std::to_string(rank + 1),
                               std::to_string(e.score) + (e.name.empty() ? "" : " " + e.name));
                }
            }
        }

        for (const auto &[name, entry] : kept)
            if (Section &s = out.section(name); !s.get(entry.first))
                s.set(entry.first, entry.second);
        for (Section &s : out.sections)
            if (!s.values.empty())
                store.sections.push_back(std::move(s));
    }

    Save fromStore(const Store &store, const Save &fresh, const Games &games)
    {
        Save save = fresh;
        const Keys keys(fresh, games);
        int unknown = 0;
        for (const Section &section : store.sections)
            if (owned(games, section.name))
                for (const auto &[key, value] : section.values)
                    unknown += !apply(save, keys, section.name, key, value);
        if (const Section *shared = store.find(kOwnSection))
            if (const std::string *name = shared->get("default_name"))
                save.defaultName = *name;
        if (unknown)
            std::cout << "[save] " << unknown << " " << games.own << " entries this build does not know, kept for one that does"
                      << std::endl;
        return save;
    }
}
