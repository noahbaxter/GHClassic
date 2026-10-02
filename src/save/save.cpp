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
// The old card folder's save is read when there is no save.bin, and becomes
// save.bin at once.

#include "save/save.h"

#include "guest.h"
#include "hook.h"
#include "save/gh2_text.h"
#include "save/store.h"
#include "settings.h"

#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>

namespace gh2::save
{
    namespace
    {
        constexpr uint32_t kBufferSize = 0x21c00u;
        constexpr uint32_t kStreamSize = 0x40u; // a BufStream; GHMCSaveData's stack holds one in 0x20

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
        Store s_store;
        Loaded s_loaded = Loaded::kNothing;
        std::string s_loadedFrom;
        std::optional<gh2::Save> s_fresh;

        std::string path()
        {
            return s_path.empty() ? settings::userDataPath("save.bin") : s_path;
        }

        std::string guestString(uint8_t *rdram, uint32_t pointer)
        {
            return reinterpret_cast<const char *>(getMemPtr(rdram, load<uint32_t>(rdram, pointer)));
        }

        // OptionData::Save's (0x10d330) fields that settings.ini holds. A load
        // takes settings.ini's, seeding any it lacks from the save; a save
        // hands back what the options menu changed. Widescreen and the sync
        // offset are video_options.cpp's and latency.cpp's.
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

        // Campaign::Save into a buffer of our own, as GHMCSaveData does.
        std::optional<gh2::Save> saveCampaign(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const auto call = [&](uint32_t function, std::initializer_list<uint32_t> args) {
                return static_cast<uint32_t>(runtime->callGuestFunction(rdram, ctx, function, args));
            };
            const uint32_t buffer = call(s_addresses->builtinNew, {kBufferSize});
            const uint32_t stream = call(s_addresses->builtinNew, {kStreamSize});
            call(s_addresses->bufStreamCtor, {stream, buffer, kBufferSize, 1u});
            // The vtable's Save slot (+0x40): a this adjustment, then the function.
            const uint32_t campaign = load<uint32_t>(rdram, s_addresses->theCampaign);
            const uint32_t vtable = load<uint32_t>(rdram, campaign);
            const uint32_t self = campaign + static_cast<uint32_t>(static_cast<int32_t>(load<int16_t>(rdram, vtable + 0x40u)));
            call(load<uint32_t>(rdram, vtable + 0x44u), {self, stream});
            call(s_addresses->binStreamDtor, {stream, 2u});
            std::optional<gh2::Save> save = gh2::parse(getMemPtr(rdram, buffer), kBufferSize);
            call(s_addresses->builtinDelete, {stream});
            call(s_addresses->builtinDelete, {buffer});
            return save;
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
            gh2::toStore(*save, *s_fresh, s_store);
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
            finish(ctx, kNoError);
        }

        // The old card folder's save into the load buffer, when there is no
        // save.bin; a damaged one gets the game's dialog instead.
        bool readCardSave(uint8_t *rdram, ReadResult read)
        {
            if (read != ReadResult::kMissing)
                return false;
            const std::filesystem::path card = PS2Runtime::getIoPaths().mcRoot /
                                               guestString(rdram, s_addresses->mcBaseDir) /
                                               guestString(rdram, s_addresses->mcSaveFile);
            std::ifstream in(card, std::ios::binary);
            if (!in)
                return false;
            const std::vector<uint8_t> data(std::istreambuf_iterator<char>(in), {});
            s_loadedFrom = card.string();
            std::memcpy(getMemPtr(rdram, load<uint32_t>(rdram, s_addresses->mcBuffer)), data.data(),
                        std::min<size_t>(data.size(), kBufferSize));
            return true;
        }

        void loadData1(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            s_loaded = Loaded::kNothing;
            Store store;
            const ReadResult read = readFile(path(), store);
            if (read == ReadResult::kOk)
                s_store = std::move(store);
            if (readCardSave(rdram, read))
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

        // A card save becomes save.bin at once.
        void storeCardSave(const gh2::Save &save)
        {
            gh2::toStore(save, *s_fresh, s_store);
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
                s_fresh = saveCampaign(rdram, ctx, runtime);
            if (GPR_S32((&saved), 4) == kNoError && s_loaded != Loaded::kNothing)
            {
                uint8_t *buffer = getMemPtr(rdram, load<uint32_t>(rdram, s_addresses->mcBuffer));
                std::optional<gh2::Save> save;
                if (s_loaded == Loaded::kCard)
                    save = gh2::parse(buffer, kBufferSize);
                else if (s_fresh)
                    save = gh2::fromStore(s_store, *s_fresh);
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
                }
            }
            *ctx = saved;
        }
    }

    void usePath(const std::string &path)
    {
        s_path = path;
    }

    void install(PS2Runtime &runtime, const Addresses &addresses)
    {
        s_addresses = &addresses;
        runtime.replaceFunction(addresses.saveData1, saveData1);
        runtime.replaceFunction(addresses.loadData1, loadData1);
        EntryHook<LoadData2Tag>::install(runtime, addresses.loadData2, onLoadData2);
    }
}
