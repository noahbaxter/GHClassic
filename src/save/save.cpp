// The save, kept in save.bin instead of on the memory card: the career of
// the game being played (content/campaigns.h) under that game's sections,
// every other game's left as they are.
//
// GHMCSaveData (retail 0x14b240) has Campaign::Save fill a 0x21c00-byte
// buffer, then SaveData1 writes it to the card on a worker thread.
// GHMCLoadData (0x14b9b0) reads it back in LoadData1, and LoadData2 hands it
// to Campaign::Load. Both card halves are replaced: a save turns the buffer
// into store sections, a load turns them back into a buffer. The menus, their
// result codes and Campaign stay the game's.
//
// The store keeps only what differs from a fresh campaign, the one the game
// builds at boot. LoadData2's first call saves that campaign once as the base
// every later load and save measures against, and a campaign switch takes the
// rebuilt Campaign as the new game's (enterGame).
//
// The game's card holds nothing (save/card.cpp). --import-card's PCSX2 card
// is read in place of save.bin by the first load, and becomes save.bin at once.
// --export-card writes the save to a PCSX2 card after the load and each save,
// as the game would have: the save, its icon and the marker file.
//
// Other games' songs get rows in GH2's high score table (HighScoreDB, built
// from a song list at 0x13cef0), starting on their own game's names: the
// fresh campaign takes them, and so does the game, through a load of it.

#include "save/save.h"

#include "guest.h"
#include "hook.h"
#include "save/gh1_stream.h"
#include "save/gh2_text.h"
#include "save/ps2_card.h"
#include "save/store.h"
#include "script.h"
#include "settings/ini.h"
#include "settings/settings.h"

#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <utility>

namespace gh2::save
{
    namespace
    {
        constexpr uint32_t kBufferSize = 0x21c00u;
        constexpr uint32_t kStreamSize = 0x40u; // a BufStream; GHMCSaveData's stack holds one in 0x20
        constexpr uint32_t kIconSysSize = 964;  // sceMcIconSys

        // ui/mem_card.dta's codes. Any load error but these two asks to make a save.
        constexpr int32_t kNoError = 0;
        constexpr int32_t kReadWriteFailed = -104;
        constexpr int32_t kCorrupt = -105;
        constexpr int32_t kFileExists = -120;

        enum class Loaded
        {
            kNothing,
            kStore,
            kCard,
        };

        const Addresses *s_addresses = nullptr;
        std::string s_path;
        std::string s_importCard;
        std::string s_exportCard;
        bool s_inMemory = false; // a save goes no further than s_store
        Store s_store;
        Loaded s_loaded = Loaded::kNothing;
        std::string s_loadedFrom;
        std::optional<gh2::Save> s_fresh;
        gh2::SongGames s_songGames;
        std::string s_game = "gh2"; // whose career the Campaign holds
        std::map<std::string, std::array<std::string, 5>> s_scoreNames; // by game
        std::vector<CardGame> s_cardGames;
        // An import's high scores from the other games' folders, by song.
        using Lists = std::array<std::array<gh2::ScoreEntry, gh2::kScoresPerList>, gh2::kDifficulties>;
        std::map<std::string, Lists> s_imported;

        std::string path()
        {
            return s_path.empty() ? settings::userDataPath("save.bin") : s_path;
        }

        // state.ini, beside save.bin, or beside --save's file so a run keeps
        // its own: [game] last, the game last entered. Written at every
        // switch, where the save holds it only once the game saves again,
        // and read before the boot loads the save.
        std::string statePath()
        {
            return s_path.empty() ? settings::userDataPath("state.ini")
                                  : (std::filesystem::path(s_path).parent_path() / "state.ini").string();
        }

        std::string readLastGame()
        {
            std::ifstream in(statePath());
            std::string line, section;
            while (std::getline(in, line))
            {
                line = ini::trim(line);
                if (ini::skipped(line))
                    continue;
                if (const auto name = ini::sectionName(line))
                    section = *name;
                else if (const size_t eq = line.find('='); section == "game" && eq != std::string::npos &&
                                                           ini::trim(line.substr(0, eq)) == "last")
                    return ini::trim(line.substr(eq + 1));
            }
            return "";
        }

        gh2::Games games()
        {
            return {s_game, s_songGames};
        }

        // --export-card's, else settings.ini's export_card puts GHClassic.ps2
        // beside save.bin. Empty when there is none.
        std::string exportPath()
        {
            if (!s_exportCard.empty() || !settings::get(settings::kExportCard))
                return s_exportCard;
            return (std::filesystem::path(path()).parent_path() / "GHClassic.ps2").string();
        }

        std::string guestString(uint8_t *rdram, uint32_t pointer)
        {
            return reinterpret_cast<const char *>(getMemPtr(rdram, load<uint32_t>(rdram, pointer)));
        }

        // OptionData::Save's (0x10d330) fields that settings.ini holds. A load
        // takes settings.ini's, seeding any it lacks from the save; a save
        // hands back what the options menu changed. Widescreen and the sync
        // offset are settings/video_options.cpp's and settings/latency.cpp's.
        struct OptionField
        {
            size_t offset;
            size_t size;
            settings::Key key;
        };
        constexpr OptionField kOptionFields[] = {
            {4, 1, settings::kLefty},        {5, 1, settings::kLeftyP2},    {6, 4, settings::kBandVolume},
            {10, 4, settings::kGuitarVolume}, {14, 4, settings::kFxVolume}, {18, 1, settings::kStereo},
        };

        int32_t optionValue(const std::array<uint8_t, gh2::kOptionsSize> &options, const OptionField &field)
        {
            int32_t value = 0;
            std::memcpy(&value, options.data() + field.offset, field.size);
            return value;
        }

        void applyOptions(std::array<uint8_t, gh2::kOptionsSize> &options)
        {
            for (const OptionField &field : kOptionFields)
            {
                if (!settings::isSet(field.key))
                    settings::set(field.key, optionValue(options, field));
                const int32_t value = settings::get(field.key);
                std::memcpy(options.data() + field.offset, &value, field.size);
            }
        }

        void keepOptions(const std::array<uint8_t, gh2::kOptionsSize> &options)
        {
            for (const OptionField &field : kOptionFields)
                if (optionValue(options, field) != settings::get(field.key))
                    settings::set(field.key, optionValue(options, field));
        }

        void finish(R5900Context *ctx, int32_t result)
        {
            setReturnS32(ctx, result);
            ctx->pc = GPR_U32(ctx, 31);
        }

        // Campaign's Save (vtable slot +0x40) or Load (+0x50) over a buffer
        // of our own, as GHMCSaveData and LoadData2 do. A slot is a this
        // adjustment, then the function. `bytes` go in first and come back out.
        void runCampaign(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t slot,
                         std::vector<uint8_t> &bytes)
        {
            const auto call = [&](uint32_t function, std::initializer_list<uint32_t> args) {
                return static_cast<uint32_t>(runtime->callGuestFunction(rdram, ctx, function, args));
            };
            const uint32_t buffer = call(s_addresses->builtinNew, {kBufferSize});
            // Zeroed: a Save that writes nothing must leave nothing to parse.
            std::memset(getMemPtr(rdram, buffer), 0, kBufferSize);
            std::memcpy(getMemPtr(rdram, buffer), bytes.data(), std::min<size_t>(bytes.size(), kBufferSize));
            const uint32_t stream = call(s_addresses->builtinNew, {kStreamSize});
            call(s_addresses->bufStreamCtor, {stream, buffer, kBufferSize, 1u});
            const uint32_t campaign = load<uint32_t>(rdram, s_addresses->theCampaign);
            const uint32_t vtable = load<uint32_t>(rdram, campaign);
            const uint32_t self =
                campaign + static_cast<uint32_t>(static_cast<int32_t>(load<int16_t>(rdram, vtable + slot)));
            call(load<uint32_t>(rdram, vtable + slot + 4u), {self, stream});
            call(s_addresses->binStreamDtor, {stream, 2u});
            const uint8_t *out = getMemPtr(rdram, buffer);
            bytes.assign(out, out + kBufferSize);
            call(s_addresses->builtinDelete, {stream});
            call(s_addresses->builtinDelete, {buffer});
        }

        std::optional<gh2::Save> saveCampaign(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            std::vector<uint8_t> bytes;
            runCampaign(rdram, ctx, runtime, 0x40u, bytes);
            return gh2::parse(bytes.data(), bytes.size());
        }

        void loadCampaign(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, const gh2::Save &save)
        {
            std::vector<uint8_t> bytes = gh2::write(save);
            runCampaign(rdram, ctx, runtime, 0x50u, bytes);
        }

        // Other games' songs start on their own game's names.
        void nameDefaults(gh2::Save &fresh)
        {
            for (gh2::SongScores &song : fresh.scores)
                if (const auto game = s_songGames.find(song.song); game != s_songGames.end())
                    for (auto &list : song.lists)
                        for (int rank = 0; rank < gh2::kScoresPerList; ++rank)
                            list[rank].name = s_scoreNames[game->second][rank];
        }

        // The song list HighScoreDB is built from (Campaign's constructor,
        // 0x12cf00) gains the other games' songs: every one the campaign's
        // own list lacks.
        struct HighScoreDbTag;
        void onHighScoreDb(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const R5900Context saved = *ctx;
            const uint32_t songs = GPR_U32(ctx, 5);
            const uint32_t scratch = GPR_U32(ctx, 29) - 0x20u;
            SET_GPR_U32(ctx, 29, scratch - 0x10u);
            std::set<std::string> listed;
            for (uint32_t at = load<uint32_t>(rdram, songs); at < load<uint32_t>(rdram, songs + 4u); at += 4u)
                listed.insert(reinterpret_cast<const char *>(getMemPtr(rdram, load<uint32_t>(rdram, at))));
            for (const auto &[song, game] : s_songGames)
            {
                if (listed.count(song) != 0u)
                    continue;
                const uint32_t symbol = script::symbol(rdram, ctx, runtime, song);
                pushBack(rdram, ctx, runtime, songs, &symbol, 4u, s_addresses->symbolsInsertOverflow, scratch);
            }
            *ctx = saved;
        }

        std::string gameOf(const std::string &song)
        {
            const auto it = s_songGames.find(song);
            return it == s_songGames.end() ? s_game : it->second;
        }

        // A game's save folder as each game writes it: the marker file named
        // after it (holding "dummydata"), the save padded to its size, then
        // icon.sys and the icon.
        std::vector<Ps2Card::File> saveFolder(const std::string &dir, const std::string &saveFile,
                                              std::vector<uint8_t> stream, uint32_t size,
                                              std::vector<uint8_t> iconSys, const std::string &iconFile,
                                              std::vector<uint8_t> icon)
        {
            stream.resize(size, 0);
            return {
                {dir, {'d', 'u', 'm', 'm', 'y', 'd', 'a', 't', 'a', 0}},
                {saveFile, std::move(stream)},
                {"icon.sys", std::move(iconSys)},
                {iconFile, std::move(icon)},
            };
        }

        // `game`'s save for its folder: the one already there, or one of no
        // progress, with its songs' high scores from `save`. GH1 keeps a table
        // per band, and each gets the same.
        std::vector<uint8_t> otherSave(const CardGame &game, const gh2::Save &save,
                                       const std::optional<std::vector<uint8_t>> &there)
        {
            if (game.gh1Layout)
            {
                std::optional<gh1::Save> out = there ? gh1::parse(there->data(), there->size()) : std::nullopt;
                if (!out)
                    out = gh1::blank();
                for (const gh2::SongScores &song : save.scores)
                {
                    if (gameOf(song.song) != game.game)
                        continue;
                    auto it = std::find_if(out->scores.begin(), out->scores.end(),
                                           [&](const gh1::SongScores &s) { return s.song == song.song; });
                    if (it == out->scores.end())
                        it = out->scores.insert(out->scores.end(), gh1::SongScores{song.song, {}});
                    for (int band = 0; band < gh1::kProfiles; ++band)
                        for (int d = 0; d < gh1::kDifficulties; ++d)
                            for (int rank = 0; rank < gh1::kScoresPerList; ++rank)
                                it->lists[band * gh1::kDifficulties + d][rank] = {song.lists[d][rank].name,
                                                                                  song.lists[d][rank].score};
                }
                return gh1::write(*out);
            }
            std::optional<gh2::Save> out = there ? gh2::parse(there->data(), there->size()) : std::nullopt;
            if (!out)
                out = gh2::blank();
            for (const gh2::SongScores &song : save.scores)
            {
                if (gameOf(song.song) != game.game)
                    continue;
                auto it = std::find_if(out->scores.begin(), out->scores.end(),
                                       [&](const gh2::SongScores &s) { return s.song == song.song; });
                if (it == out->scores.end())
                    out->scores.push_back(song);
                else
                    it->lists = song.lists;
            }
            return gh2::write(*out);
        }

        // Every game's folder, GH2's with the icon SetupMCIcon (0x14b098) last
        // built. The game whose career `save` is takes it whole; each other
        // takes its songs' high scores.
        void writeCard(uint8_t *rdram, const gh2::Save &save, const std::string &to)
        {
            std::optional<Ps2Card> card = Ps2Card::open(to);
            if (!card)
            {
                std::error_code error;
                if (std::filesystem::exists(to, error))
                {
                    std::cerr << "[save] " << to << " is not an 8 MB PS2 card; not exported" << std::endl;
                    return;
                }
                card = Ps2Card::format();
            }
            const uint32_t icon = load<uint32_t>(rdram, s_addresses->mcIconData);
            if (icon == 0u)
            {
                std::cerr << "[save] no card icon built; not exported" << std::endl;
                return;
            }
            const uint8_t *iconSys = getMemPtr(rdram, s_addresses->mcIconSys);
            const uint8_t *iconData = getMemPtr(rdram, icon);
            const std::string saveFile = guestString(rdram, s_addresses->mcSaveFile);
            // Its own songs alone: the game would drop the rest on loading.
            gh2::Save own = save;
            std::erase_if(own.scores, [](const gh2::SongScores &s) { return gameOf(s.song) != s_game; });
            std::vector<CardGame> folders = {{"gh2", guestString(rdram, s_addresses->mcBaseDir), false, kBufferSize,
                                              {iconSys, iconSys + kIconSysSize}, guestString(rdram, s_addresses->mcIconFile),
                                              {iconData, iconData + load<uint32_t>(rdram, s_addresses->mcIconSize)}}};
            folders.insert(folders.end(), s_cardGames.begin(), s_cardGames.end());
            bool ok = true;
            for (const CardGame &game : folders)
            {
                // GH1's own layout holds only what otherSave gives it.
                const std::vector<uint8_t> stream = game.game == s_game && !game.gh1Layout
                                                        ? gh2::write(own)
                                                        : otherSave(game, save, card->read(game.dir, saveFile));
                if (stream.size() > game.dataSize)
                {
                    std::cerr << "[save] " << game.game << "'s save does not fit its card file; not exported" << std::endl;
                    continue;
                }
                ok = ok && card->replaceDir(game.dir, saveFolder(game.dir, saveFile, stream, game.dataSize, game.iconSys,
                                                                 game.iconFile, game.icon));
            }
            if (!ok || !card->save(to))
                std::cerr << "[save] could not write " << to << std::endl;
            else
                std::cout << "[save] exported to " << to << std::endl;
        }

        void saveData1(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            const uint32_t buffer = load<uint32_t>(rdram, s_addresses->mcBuffer);
            const std::optional<gh2::Save> save = gh2::parse(getMemPtr(rdram, buffer), kBufferSize);
            // All access on: Campaign::Save wrote nothing, and the card would
            // have taken the buffer's garbage.
            if (!save)
                return finish(ctx, kNoError);
            if (!s_fresh)
            {
                std::cerr << "[save] no fresh campaign to measure against; not saved" << std::endl;
                return finish(ctx, kReadWriteFailed);
            }
            if (s_inMemory)
            {
                keepOptions(save->options);
                gh2::toStore(*save, *s_fresh, s_store, games());
                std::cout << "[save] kept in memory, not written" << std::endl;
                return finish(ctx, kNoError);
            }
            // As retail's (0x14b514): a save already there is replaced only once
            // the player said so, which the menus ask for on this code.
            Store old;
            const ReadResult there = readFile(path(), old);
            if (there != ReadResult::kMissing && load<uint32_t>(rdram, s_addresses->mcOverwrite) == 0u)
                return finish(ctx, kFileExists);
            keepOptions(save->options);
            gh2::toStore(*save, *s_fresh, s_store, games());
            // A damaged save is only ever replaced by choice, and kept aside.
            if (there == ReadResult::kCorrupt)
            {
                std::error_code error;
                std::filesystem::rename(path(), path() + ".bad", error);
                std::cerr << "[save] the damaged save is now " << path() << ".bad" << std::endl;
            }
            if (!writeFile(path(), s_store))
            {
                std::cerr << "[save] could not write " << path() << std::endl;
                return finish(ctx, kReadWriteFailed);
            }
            if (const std::string to = exportPath(); !to.empty())
                writeCard(rdram, *save, to);
            finish(ctx, kNoError);
        }

        // --import-card's save into the load buffer, once: a later load in
        // the same run is save.bin's.
        bool readCardSave(uint8_t *rdram)
        {
            // The save read here is GH2's career.
            if (s_importCard.empty() || s_game != "gh2")
                return false;
            s_loadedFrom = std::exchange(s_importCard, {});
            const std::string dir = guestString(rdram, s_addresses->mcBaseDir);
            const std::string file = guestString(rdram, s_addresses->mcSaveFile);
            const std::optional<Ps2Card> card = Ps2Card::open(s_loadedFrom);
            const std::optional<std::vector<uint8_t>> found = card ? card->read(dir, file) : std::nullopt;
            if (!found)
            {
                std::cerr << "[save] no " << dir << "/" << file << " on " << s_loadedFrom << std::endl;
                return false;
            }
            std::memcpy(getMemPtr(rdram, load<uint32_t>(rdram, s_addresses->mcBuffer)), found->data(),
                        std::min<size_t>(found->size(), kBufferSize));
            return true;
        }

        // GH1 keeps a table per band; a song takes its best five across them.
        Lists bestOfBands(const gh1::SongScores &song)
        {
            Lists lists;
            for (int d = 0; d < gh1::kDifficulties; ++d)
            {
                std::vector<gh2::ScoreEntry> all;
                for (int band = 0; band < gh1::kProfiles; ++band)
                    for (const gh1::ScoreEntry &e : song.lists[band * gh1::kDifficulties + d])
                        if (std::none_of(all.begin(), all.end(), [&](const gh2::ScoreEntry &a) { return a == gh2::ScoreEntry{e.name, e.score}; }))
                            all.push_back({e.name, e.score});
                std::stable_sort(all.begin(), all.end(), [](const auto &a, const auto &b) { return a.score > b.score; });
                for (int rank = 0; rank < gh1::kScoresPerList && rank < static_cast<int>(all.size()); ++rank)
                    lists[d][rank] = all[rank];
            }
            return lists;
        }

        // An import's other games: each one's folder on the card gives its
        // songs' high scores.
        void readImportedScores(uint8_t *rdram)
        {
            s_imported.clear();
            const std::optional<Ps2Card> card = Ps2Card::open(s_importCard);
            if (!card)
                return;
            const std::string saveFile = guestString(rdram, s_addresses->mcSaveFile);
            for (const CardGame &game : s_cardGames)
            {
                const std::optional<std::vector<uint8_t>> data = card->read(game.dir, saveFile);
                if (!data)
                    continue;
                size_t songs = 0;
                bool parsed = false;
                if (game.gh1Layout)
                {
                    if (const std::optional<gh1::Save> save = gh1::parse(data->data(), data->size()))
                    {
                        parsed = true;
                        for (const gh1::SongScores &song : save->scores)
                            if (gameOf(song.song) == game.game)
                                s_imported[song.song] = bestOfBands(song), ++songs;
                    }
                }
                else if (const std::optional<gh2::Save> save = gh2::parse(data->data(), data->size()))
                {
                    parsed = true;
                    for (const gh2::SongScores &song : save->scores)
                        if (gameOf(song.song) == game.game)
                            s_imported[song.song] = song.lists, ++songs;
                }
                if (parsed)
                    std::cout << "[save] " << game.game << ": high scores for " << songs << " songs from "
                              << s_importCard << std::endl;
                else
                    std::cerr << "[save] " << game.dir << " on " << s_importCard << " is damaged" << std::endl;
            }
        }

        void loadData1(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            s_loaded = Loaded::kNothing;
            Store store;
            const ReadResult read = readFile(path(), store);
            if (read == ReadResult::kOk)
                s_store = std::move(store);
            if (!s_importCard.empty())
                readImportedScores(rdram);
            if (readCardSave(rdram))
            {
                s_loaded = Loaded::kCard;
                return finish(ctx, kNoError);
            }
            // Only the other games' scores on the card: over save.bin, or a
            // fresh campaign.
            if (read == ReadResult::kOk || (read == ReadResult::kMissing && !s_imported.empty()))
            {
                s_loaded = Loaded::kStore;
                if (!s_imported.empty())
                    s_loadedFrom = s_importCard;
                return finish(ctx, kNoError);
            }
            switch (read)
            {
            case ReadResult::kOk:
                break;
            case ReadResult::kCorrupt:
                // The game's damaged-save dialog, which offers to make a new one.
                std::cerr << "[save] " << path() << " is damaged" << std::endl;
                return finish(ctx, kCorrupt);
            case ReadResult::kMissing:
                break;
            }
            finish(ctx, kReadWriteFailed);
        }

        // A card save is GH2's alone: it takes GH2's progress and the scores of
        // the songs it has, and the other games' scores stay.
        void overCard(gh2::Save &save, const gh2::Save &card)
        {
            std::vector<gh2::SongScores> scores = card.scores;
            for (const gh2::SongScores &song : save.scores)
                if (std::none_of(scores.begin(), scores.end(),
                                 [&](const gh2::SongScores &s) { return s.song == song.song; }))
                    scores.push_back(song);
            save = card;
            save.scores = std::move(scores);
        }

        // A card save becomes save.bin at once, the old one kept as save.bin.bak.
        void storeCardSave(const gh2::Save &save)
        {
            if (s_inMemory)
            {
                gh2::toStore(save, *s_fresh, s_store, games());
                return;
            }
            std::error_code error;
            if (std::filesystem::exists(path(), error))
                std::filesystem::copy_file(path(), path() + ".bak", std::filesystem::copy_options::overwrite_existing,
                                           error);
            gh2::toStore(save, *s_fresh, s_store, games());
            if (writeFile(path(), s_store))
                std::cout << "[save] " << s_loadedFrom << " is now " << path() << std::endl;
            else
                std::cerr << "[save] could not write " << path() << std::endl;
        }

        // Guest calls clobber the hooked call's arguments, so the context goes back.
        struct LoadData2Tag;
        void onLoadData2(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            R5900Context saved = *ctx;
            if (!s_fresh)
            {
                s_fresh = saveCampaign(rdram, ctx, runtime);
                // The other games' rows take their names in the game too, so
                // a save without them leaves them right.
                if (s_fresh && !s_songGames.empty())
                {
                    nameDefaults(*s_fresh);
                    gh2::Save boot = *s_fresh;
                    applyOptions(boot.options);
                    loadCampaign(rdram, ctx, runtime, boot);
                }
            }
            if (GPR_S32((&saved), 4) == kNoError && s_loaded != Loaded::kNothing)
            {
                uint8_t *buffer = getMemPtr(rdram, load<uint32_t>(rdram, s_addresses->mcBuffer));
                std::optional<gh2::Save> save;
                if (s_fresh)
                    save = gh2::fromStore(s_store, *s_fresh, games());
                if (save && s_loaded == Loaded::kCard)
                {
                    if (const std::optional<gh2::Save> card = gh2::parse(buffer, kBufferSize))
                        overCard(*save, *card);
                    else
                        save.reset();
                }
                if (save)
                    for (gh2::SongScores &song : save->scores)
                        if (const auto it = s_imported.find(song.song); it != s_imported.end())
                            song.lists = it->second;
                std::vector<uint8_t> bytes;
                if (save)
                {
                    applyOptions(save->options);
                    bytes = gh2::write(*save);
                }
                if (bytes.empty() || bytes.size() > kBufferSize)
                {
                    std::cerr << "[save] could not load the save" << std::endl;
                    SET_GPR_S32((&saved), 4, kCorrupt);
                }
                else
                {
                    std::memcpy(buffer, bytes.data(), bytes.size());
                    if ((s_loaded == Loaded::kCard || !s_imported.empty()) && s_fresh)
                        storeCardSave(*save);
                    if (const std::string to = exportPath(); !to.empty())
                    {
                        runtime->callGuestFunction(rdram, ctx, s_addresses->setupMcIcon, {});
                        writeCard(rdram, *save, to);
                    }
                }
            }
            *ctx = saved;
        }
    }

    namespace
    {
        const char *const kDifficulties[gh2::kDifficulties] = {"easy", "medium", "hard", "expert"};
        // Each "song.<name>" in a section whose state has a word starting
        // `start` (gh2_text.h): "unlocked", or "stars=", which a song has
        // once it is finished.
        void songsWith(const Section &section, const std::string &start, std::set<std::string> &out)
        {
            for (const auto &[key, value] : section.values)
            {
                if (key.rfind("song.", 0) != 0)
                    continue;
                std::istringstream words(value);
                for (std::string word; words >> word;)
                    if (word.rfind(start, 0) == 0)
                        out.insert(key.substr(5, key.find('#') == std::string::npos ? std::string::npos : key.find('#') - 5));
            }
        }
    }

    std::array<std::set<std::string>, 4> passedSongs(const std::string &game, int slot)
    {
        std::array<std::set<std::string>, 4> passed;
        for (int d = 0; d < gh2::kDifficulties; ++d)
            if (const Section *section = s_store.find("band " + std::to_string(slot + 1) + " " + game + " " + kDifficulties[d]))
                songsWith(*section, "stars=", passed[static_cast<size_t>(d)]);
        return passed;
    }

    std::set<std::string> unlockedSongs(const std::string &game)
    {
        std::set<std::string> unlocked;
        for (int slot = 0; slot < gh2::kProfiles; ++slot)
            for (const char *difficulty : kDifficulties)
                if (const Section *section = s_store.find("band " + std::to_string(slot + 1) + " " + game + " " + difficulty))
                    songsWith(*section, "unlocked", unlocked);
        return unlocked;
    }

    bool unlockedItem(const std::string &game, const std::string &item)
    {
        const std::string own = " " + game;
        for (const Section &section : s_store.sections)
        {
            // [band N <game>] and [band N <game> <difficulty>].
            const size_t at = section.name.find(own);
            if (section.name.rfind("band ", 0) != 0 || at == std::string::npos ||
                (at + own.size() != section.name.size() && section.name[at + own.size()] != ' '))
                continue;
            for (const auto &[key, value] : section.values)
            {
                // "<type>.<item>", "#2" on for one listed twice.
                const size_t dot = key.find('.');
                if (dot == std::string::npos || key.substr(dot + 1, key.find('#') - dot - 1) != item)
                    continue;
                std::istringstream words(value);
                for (std::string word; words >> word;)
                    if (word == "unlocked")
                        return true;
            }
        }
        return false;
    }

    void leaveGame(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (!s_fresh)
            return;
        // Nothing with all access on, which no save holds either.
        if (const std::optional<gh2::Save> save = saveCampaign(rdram, ctx, runtime))
        {
            keepOptions(save->options);
            gh2::toStore(*save, *s_fresh, s_store, games());
        }
    }

    std::string lastGame()
    {
        if (std::string game = readLastGame(); !game.empty())
            return game;
        // Before the boot's load, the save as it is on disk.
        Store disk;
        const Store &store = s_store.find(kOwnSection) || readFile(path(), disk) != ReadResult::kOk ? s_store : disk;
        const Section *section = store.find(kOwnSection);
        const std::string *game = section ? section->get("campaign") : nullptr;
        return game ? *game : "gh2";
    }

    void enterGame(const std::string &game, uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        s_game = game;
        s_store.section(kOwnSection).set("campaign", game);
        if (!s_inMemory)
            if (ini::writeReplacing(statePath(), [&](std::ofstream &out) { out << "[game]\nlast = " << game << "\n"; }))
                std::cerr << "[save] could not write " << statePath() << std::endl;
        s_fresh = saveCampaign(rdram, ctx, runtime);
        if (!s_fresh)
        {
            std::cerr << "[save] no fresh " << game << " campaign to measure against" << std::endl;
            return;
        }
        nameDefaults(*s_fresh);
        gh2::Save save = gh2::fromStore(s_store, *s_fresh, games());
        applyOptions(save.options);
        loadCampaign(rdram, ctx, runtime, save);
    }

    void usePath(const std::string &path)
    {
        s_path = path;
    }

    void keepInMemory()
    {
        s_inMemory = true;
    }

    void importCard(const std::string &path)
    {
        s_importCard = path;
    }

    void exportCard(const std::string &path)
    {
        s_exportCard = path;
    }

    void addCardGame(CardGame game)
    {
        s_cardGames.push_back(std::move(game));
    }

    void addScoreSongs(const std::string &game, const std::vector<std::string> &songs,
                       const std::array<std::string, 5> &names)
    {
        for (const std::string &song : songs)
            s_songGames[song] = game;
        s_scoreNames[game] = names;
    }

    void install(PS2Runtime &runtime, const Addresses &addresses)
    {
        s_addresses = &addresses;
        runtime.replaceFunction(addresses.saveData1, saveData1);
        runtime.replaceFunction(addresses.loadData1, loadData1);
        EntryHook<LoadData2Tag>::install(runtime, addresses.loadData2, onLoadData2);
        EntryHook<HighScoreDbTag>::install(runtime, addresses.highScoreDbCtor, onHighScoreDb);
    }
}
