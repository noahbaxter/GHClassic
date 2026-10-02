// GH2's save, kept in save.bin instead of on the memory card.
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
// every later load and save measures against.
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
#include "save/gh2_text.h"
#include "save/ps2_card.h"
#include "save/store.h"
#include "script.h"
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
        Store s_store;
        Loaded s_loaded = Loaded::kNothing;
        std::string s_loadedFrom;
        std::optional<gh2::Save> s_fresh;
        gh2::SongGames s_songGames;
        std::map<std::string, std::array<std::string, 5>> s_scoreNames; // by game

        std::string path()
        {
            return s_path.empty() ? settings::userDataPath("save.bin") : s_path;
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
        // 0x12cf00) gains the other games' songs.
        struct HighScoreDbTag;
        void onHighScoreDb(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const R5900Context saved = *ctx;
            const uint32_t songs = GPR_U32(ctx, 5);
            const uint32_t scratch = GPR_U32(ctx, 29) - 0x20u;
            SET_GPR_U32(ctx, 29, scratch - 0x10u);
            for (const auto &[song, game] : s_songGames)
            {
                const uint32_t symbol = script::symbol(rdram, ctx, runtime, song);
                pushBack(rdram, ctx, runtime, songs, &symbol, 4u, s_addresses->symbolsInsertOverflow, scratch);
            }
            *ctx = saved;
        }

        // The game's card directory: the marker file named after it (holding
        // "dummydata"), the save padded to the card's size, and the icon
        // SetupMCIcon (0x14b098) last built.
        void writeCard(uint8_t *rdram, const std::vector<uint8_t> &stream, const std::string &to)
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
            const std::string dir = guestString(rdram, s_addresses->mcBaseDir);
            std::vector<uint8_t> data(kBufferSize, 0);
            std::copy(stream.begin(), stream.end(), data.begin());
            const std::vector<Ps2Card::File> files = {
                {dir, {'d', 'u', 'm', 'm', 'y', 'd', 'a', 't', 'a', 0}},
                {guestString(rdram, s_addresses->mcSaveFile), std::move(data)},
                {"icon.sys", {iconSys, iconSys + kIconSysSize}},
                {guestString(rdram, s_addresses->mcIconFile),
                 {iconData, iconData + load<uint32_t>(rdram, s_addresses->mcIconSize)}},
            };
            if (!card->replaceDir(dir, files) || !card->save(to))
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
            // As retail's (0x14b514): a save already there is replaced only once
            // the player said so, which the menus ask for on this code.
            Store old;
            const ReadResult there = readFile(path(), old);
            if (there != ReadResult::kMissing && load<uint32_t>(rdram, s_addresses->mcOverwrite) == 0u)
                return finish(ctx, kFileExists);
            keepOptions(save->options);
            gh2::toStore(*save, *s_fresh, s_store, s_songGames);
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
                writeCard(rdram, gh2::write(*save), to);
            finish(ctx, kNoError);
        }

        // --import-card's save into the load buffer, once: a later load in
        // the same run is save.bin's.
        bool readCardSave(uint8_t *rdram)
        {
            if (s_importCard.empty())
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

        void loadData1(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            s_loaded = Loaded::kNothing;
            Store store;
            const ReadResult read = readFile(path(), store);
            if (read == ReadResult::kOk)
                s_store = std::move(store);
            if (readCardSave(rdram))
            {
                s_loaded = Loaded::kCard;
                return finish(ctx, kNoError);
            }
            switch (read)
            {
            case ReadResult::kOk:
                s_loaded = Loaded::kStore;
                return finish(ctx, kNoError);
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
            std::error_code error;
            if (std::filesystem::exists(path(), error))
                std::filesystem::copy_file(path(), path() + ".bak", std::filesystem::copy_options::overwrite_existing,
                                           error);
            gh2::toStore(save, *s_fresh, s_store, s_songGames);
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
                    save = gh2::fromStore(s_store, *s_fresh, s_songGames);
                if (save && s_loaded == Loaded::kCard)
                {
                    if (const std::optional<gh2::Save> card = gh2::parse(buffer, kBufferSize))
                        overCard(*save, *card);
                    else
                        save.reset();
                }
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
                    if (s_loaded == Loaded::kCard && s_fresh)
                        storeCardSave(*save);
                    if (const std::string to = exportPath(); !to.empty())
                    {
                        runtime->callGuestFunction(rdram, ctx, s_addresses->setupMcIcon, {});
                        writeCard(rdram, bytes, to);
                    }
                }
            }
            *ctx = saved;
        }
    }

    void usePath(const std::string &path)
    {
        s_path = path;
    }

    void importCard(const std::string &path)
    {
        s_importCard = path;
    }

    void exportCard(const std::string &path)
    {
        s_exportCard = path;
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
