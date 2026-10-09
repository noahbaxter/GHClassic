// The game's archive, owned by the host: every disc's MAIN.HDR indexed here,
// and the three functions that turn a path into sectors replaced, so files
// can come from more than one disc, each read in place.
//
// Retail resolves a path in Archive::GetFileInfo (0x2ac618) to an ark part
// and a byte offset in it, bounds blocks with Archive::IsValidBlock
// (0x2ac9e8), and reads them through CDRead (0x2ae9c0), which calls
// sceCdRead at that part's LBA. Here part numbers run on past the game
// disc's own, one per part of each added disc, and one per loose file.

#include "disc/ark.h"

#include "disc/crypt.h"
#include "disc/volume.h"
#include "guest.h"
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "runtime/ps2_disc_image.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace gh2::ark
{
    namespace
    {
        // An ark part or one loose file: `length` bytes, read from an
        // offset.
        struct Part
        {
            std::function<size_t(uint64_t offset, uint8_t *dst, size_t size)> read;
            uint64_t length = 0u;
            uint32_t size = 0u; // from the header, what IsValidBlock bounds by
        };

        struct Entry
        {
            uint32_t part;
            uint32_t offset; // in the part
            uint32_t size;
            uint32_t uncompressedSize;
        };

        struct Disc
        {
            std::string serial; // Volume::serial's
            std::unordered_map<std::string, Entry> files; // dir/name -> entry
            Volume *volume = nullptr;
        };

        // Paths under `as` are `source`'s on one disc, and nowhere else.
        struct Rename
        {
            std::string as;
            size_t disc;
            std::string source;
        };

        std::unique_ptr<Volume> s_game;
        std::vector<std::unique_ptr<Volume>> s_added;
        std::vector<Part> s_parts;
        std::vector<Disc> s_discs; // searched in order, the game disc first
        std::vector<Rename> s_renames;
        std::unordered_map<std::string, Entry> s_loose; // searched before all else
        std::unordered_map<std::string, std::function<std::optional<Made>()>> s_made; // loose once made
        uint32_t s_blockSizeAddress = 0u;
        struct Layer
        {
            std::unordered_map<std::string, Entry> files;
            std::optional<size_t> disc;
        };
        std::vector<Layer> s_layers;
        std::optional<size_t> s_front; // the layer searched before the game disc
        std::optional<size_t> s_boot;  // bootFrom's
        std::unordered_set<std::string> s_bootFiles;

        uint32_t word(const std::vector<uint8_t> &data, size_t &at)
        {
            uint32_t value = 0u;
            if (at + 4u <= data.size())
                std::memcpy(&value, data.data() + at, 4u);
            at += 4u;
            return value;
        }

        // MAIN.HDR as Archive::Read (0x2ac8e8) reads it: version 3, the part
        // sizes, ArkHash's string buffer and slot table, then 20-byte
        // entries of byte offset over all parts, name slot, dir slot, size
        // and uncompressed size. A 360's is encrypted whole (crypt.h).
        bool index(Volume &volume, const std::string &label)
        {
            const auto header = volume.find("GEN/MAIN.HDR");
            if (!header)
            {
                std::cerr << "[ark] " << label << ": no GEN/MAIN.HDR" << std::endl;
                return false;
            }
            std::vector<uint8_t> hdr(static_cast<size_t>(header->size));
            if (volume.read(header->offset, hdr.data(), hdr.size()) != hdr.size())
            {
                std::cerr << "[ark] " << label << ": short read of MAIN.HDR" << std::endl;
                return false;
            }

            size_t at = 0u;
            if (word(hdr, at) != 3u)
                hdr = crypt::parkMiller(hdr);
            at = 0u;
            const uint32_t version = word(hdr, at);
            if (version != 3u)
            {
                std::cerr << "[ark] " << label << ": MAIN.HDR version " << version << ", not 3" << std::endl;
                return false;
            }
            word(hdr, at); // the part count again, as Archive's +0x00
            const uint32_t partCount = word(hdr, at);
            const uint32_t firstPart = static_cast<uint32_t>(s_parts.size());
            std::vector<uint32_t> partSizes;
            for (uint32_t i = 0u; i < partCount; ++i)
                partSizes.push_back(word(hdr, at));
            for (uint32_t i = 0u; i < partCount; ++i)
            {
                const std::string path = "GEN/MAIN_" + std::to_string(i) + ".ARK";
                const auto file = volume.find(path);
                if (!file)
                {
                    std::cerr << "[ark] " << label << ": no " << path << std::endl;
                    s_parts.resize(firstPart);
                    return false;
                }
                Volume *from = &volume;
                const uint64_t base = file->offset;
                s_parts.push_back({[from, base](uint64_t offset, uint8_t *dst, size_t size)
                                   { return from->read(base + offset, dst, size); },
                                   file->size, partSizes[i]});
            }

            const uint32_t stringBytes = word(hdr, at);
            const size_t strings = at;
            at += stringBytes;
            const uint32_t slotCount = word(hdr, at);
            std::vector<uint32_t> slots;
            for (uint32_t i = 0u; i < slotCount; ++i)
                slots.push_back(word(hdr, at));
            auto slotString = [&](uint32_t slot) -> std::string
            {
                if (slot >= slots.size() || slots[slot] >= stringBytes)
                    return {};
                const char *s = reinterpret_cast<const char *>(hdr.data() + strings + slots[slot]);
                return std::string(s, strnlen(s, stringBytes - slots[slot]));
            };

            std::unordered_map<std::string, Entry> files;
            const uint32_t entryCount = word(hdr, at);
            for (uint32_t i = 0u; i < entryCount && at + 20u <= hdr.size(); ++i)
            {
                uint32_t offset = word(hdr, at);
                const uint32_t name = word(hdr, at);
                const uint32_t dir = word(hdr, at);
                const uint32_t size = word(hdr, at);
                const uint32_t uncompressedSize = word(hdr, at);
                // The part holding the offset, as GetFileInfo walks it.
                uint32_t part = 0u;
                while (part + 1u < partCount && offset >= partSizes[part])
                    offset -= partSizes[part++];
                files[slotString(dir) + "/" + slotString(name)] = {firstPart + part, offset, size, uncompressedSize};
            }
            const std::string serial = volume.serial();
            std::cerr << "[ark] " << label << " (" << serial << "): " << files.size() << " files in " << partCount
                      << " part" << (partCount == 1u ? "" : "s") << std::endl;
            s_discs.push_back({serial, std::move(files), &volume});
            return true;
        }

        // FileGetPath (0x2a34e8) and FileGetName (0x2a3698): split at the
        // last '/', or the last '\' when there is none. No separator is
        // dir ".", and a separator first or after ':' stays in the dir.
        std::string key(const char *path)
        {
            const char *slash = std::strrchr(path, '/');
            if (!slash)
                slash = std::strrchr(path, '\\');
            if (!slash)
                return std::string(".") + "/" + path;
            const bool keep = slash == path || slash[-1] == ':';
            return std::string(path, slash + (keep ? 1 : 0)) + "/" + (slash + 1);
        }

        Entry addPart(Part part)
        {
            const Entry entry{static_cast<uint32_t>(s_parts.size()), 0u, part.size, 0u};
            s_parts.push_back(std::move(part));
            return entry;
        }

        // Bytes held for as long as the game may read them.
        Entry addBytes(std::vector<uint8_t> bytes)
        {
            auto held = std::make_shared<const std::vector<uint8_t>>(std::move(bytes));
            const uint32_t size = static_cast<uint32_t>(held->size());
            return addPart({[held](uint64_t offset, uint8_t *dst, size_t want)
                            {
                                const size_t n = offset < held->size()
                                                     ? std::min<size_t>(want, held->size() - offset)
                                                     : 0u;
                                std::memcpy(dst, held->data() + offset, n);
                                return n;
                            },
                            size, size});
        }

        // A layer's own file, else its disc's.
        const Entry *findIn(const Layer &layer, const std::string &name)
        {
            if (const auto it = layer.files.find(name); it != layer.files.end())
                return &it->second;
            if (layer.disc)
            {
                const auto &files = s_discs[*layer.disc].files;
                if (const auto it = files.find(name); it != files.end())
                    return &it->second;
            }
            return nullptr;
        }

        // Loose files first, made ones as they are asked for, then the boot's
        // own, then renames, then the layer in front if one is, then each
        // disc in turn.
        const Entry *find(const std::string &name)
        {
            const auto loose = s_loose.find(name);
            if (loose != s_loose.end())
                return &loose->second;
            if (const auto made = s_made.find(name); made != s_made.end())
            {
                auto file = made->second();
                s_made.erase(made);
                if (file)
                {
                    const uint32_t size = file->size;
                    return &(s_loose[name] = addPart({std::move(file->read), size, size}));
                }
            }
            if (s_boot && s_bootFiles.count(name) != 0u)
                if (const Entry *entry = findIn(s_layers[*s_boot], name))
                    return entry;
            for (const Rename &rename : s_renames)
            {
                if (name.compare(0u, rename.as.size(), rename.as) != 0)
                    continue;
                const auto &files = s_discs[rename.disc].files;
                const auto it = files.find(rename.source + name.substr(rename.as.size()));
                return it != files.end() ? &it->second : nullptr;
            }
            if (s_front)
                if (const Entry *entry = findIn(s_layers[*s_front], name))
                    return entry;
            for (const Disc &disc : s_discs)
            {
                const auto it = disc.files.find(name);
                if (it != disc.files.end())
                    return &it->second;
            }
            return nullptr;
        }

        // Archive::GetFileInfo(this, path, &part, &offset, &size,
        // &uncompressedSize), true when found.
        void getFileInfo(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            const uint32_t path = GPR_U32(ctx, 5);
            const Entry *found = nullptr;
            if (path != 0u)
            {
                const std::string name = key(reinterpret_cast<const char *>(getMemPtr(rdram, path)));
                found = find(name);
                // Retail asks only for files it has, so a miss is news.
                static std::unordered_set<std::string> s_missed;
                if (!found && s_missed.insert(name).second)
                    std::cerr << "[ark] no file " << name << std::endl;
            }
            const Entry entry = found ? *found : Entry{};
            store<uint32_t>(rdram, GPR_U32(ctx, 6), entry.part);
            store<uint32_t>(rdram, GPR_U32(ctx, 7), entry.offset);
            store<uint32_t>(rdram, GPR_U32(ctx, 8), entry.size);
            store<uint32_t>(rdram, GPR_U32(ctx, 9), entry.uncompressedSize);
            SET_GPR_U32(ctx, 2, found ? 1u : 0u);
            ctx->pc = GPR_U32(ctx, 31);
        }

        // Archive::IsValidBlock(this, part, block): block within the part.
        void isValidBlock(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            const int32_t part = static_cast<int32_t>(GPR_U32(ctx, 5));
            const int32_t block = static_cast<int32_t>(GPR_U32(ctx, 6));
            bool valid = false;
            if (part >= 0 && static_cast<size_t>(part) < s_parts.size() && block >= 0)
            {
                const uint32_t blockSize = load<uint32_t>(rdram, s_blockSizeAddress);
                valid = blockSize != 0u &&
                        static_cast<uint32_t>(block) <= (s_parts[part].size - 1u) / blockSize;
            }
            SET_GPR_U32(ctx, 2, valid ? 1u : 0u);
            ctx->pc = GPR_U32(ctx, 31);
        }

        // CDRead(part, sector, count, buffer), sectors within the part. The
        // read finishes before returning, as the runtime's sceCdRead does,
        // so CDReadDone (sceCdSync) finds it done. Returns true on failure.
        void cdRead(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            const uint32_t part = GPR_U32(ctx, 4);
            const uint64_t offset = static_cast<uint64_t>(GPR_U32(ctx, 5)) * DiscImage::kSectorSize;
            const size_t bytes = static_cast<size_t>(GPR_U32(ctx, 6)) * DiscImage::kSectorSize;
            bool failed = true;
            if (part < s_parts.size() && offset < s_parts[part].length)
            {
                const Part &p = s_parts[part];
                // The last block runs past the part's end; the disc beyond
                // it is never used, so only the file's bytes are read.
                const size_t want = std::min<uint64_t>(bytes, p.length - offset);
                failed = p.read(offset, getMemPtr(rdram, GPR_U32(ctx, 7)), want) != want;
            }
            if (failed)
                std::cerr << "[ark] read failed: part " << part << " sector " << GPR_U32(ctx, 5) << std::endl;
            SET_GPR_U32(ctx, 2, failed ? 1u : 0u);
            ctx->pc = GPR_U32(ctx, 31);
        }
    }

    bool addDisc(const std::string &path)
    {
        std::string error;
        std::unique_ptr<Volume> disc = Volume::open(path, error);
        if (!disc)
        {
            std::cerr << "[ark] " << path << ": " << error << std::endl;
            return false;
        }
        s_added.push_back(std::move(disc));
        return true;
    }

    void addFile(const std::string &path, std::vector<uint8_t> bytes)
    {
        s_loose[key(path.c_str())] = addBytes(std::move(bytes));
    }

    size_t addLayer(std::optional<size_t> disc)
    {
        s_layers.push_back({{}, disc});
        return s_layers.size() - 1u;
    }

    void addFile(size_t layer, const std::string &path, std::vector<uint8_t> bytes)
    {
        s_layers[layer].files[key(path.c_str())] = addBytes(std::move(bytes));
    }

    bool lend(size_t layer, const std::string &path, size_t disc, const std::string &source)
    {
        if (disc >= s_discs.size())
            return false;
        const auto &files = s_discs[disc].files;
        const auto it = files.find(key(source.c_str()));
        if (it == files.end())
            return false;
        s_layers[layer].files[key(path.c_str())] = it->second;
        return true;
    }

    bool lendFromDisc(size_t layer, const std::string &path, size_t disc, const std::string &source)
    {
        const auto file = disc < s_discs.size() ? s_discs[disc].volume->find(source) : std::nullopt;
        if (!file)
            return false;
        Volume *from = s_discs[disc].volume;
        const uint64_t base = file->offset;
        const uint32_t size = static_cast<uint32_t>(file->size);
        s_layers[layer].files[key(path.c_str())] =
            addPart({[from, base](uint64_t offset, uint8_t *dst, size_t want) { return from->read(base + offset, dst, want); },
                     file->size, size});
        return true;
    }

    void addMade(const std::string &path, std::function<std::optional<Made>()> make)
    {
        s_made[key(path.c_str())] = std::move(make);
    }

    bool addFolder(const std::string &root)
    {
        std::error_code error;
        std::filesystem::recursive_directory_iterator it(root, error);
        if (error)
        {
            std::cerr << "[ark] " << root << ": " << error.message() << std::endl;
            return false;
        }
        size_t count = 0u;
        for (const auto &entry : it)
        {
            if (!entry.is_regular_file())
                continue;
            std::ifstream in(entry.path(), std::ios::binary);
            std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            addFile(std::filesystem::relative(entry.path(), root).generic_string(), std::move(bytes));
            ++count;
        }
        std::cerr << "[ark] " << root << ": " << count << " loose files" << std::endl;
        return true;
    }

    namespace
    {
        std::optional<std::vector<uint8_t>> read(const Entry *entry)
        {
            if (!entry)
                return std::nullopt;
            std::vector<uint8_t> bytes(entry->size);
            if (s_parts[entry->part].read(entry->offset, bytes.data(), bytes.size()) != bytes.size())
                return std::nullopt;
            return bytes;
        }
    }

    std::optional<std::vector<uint8_t>> readFile(const std::string &path)
    {
        return read(find(key(path.c_str())));
    }

    std::optional<Made> openFront(const std::string &path)
    {
        const Entry *entry = s_front ? findIn(s_layers[*s_front], key(path.c_str())) : nullptr;
        if (!entry)
            return std::nullopt;
        const Entry at = *entry;
        return Made{at.size, [at](uint64_t offset, uint8_t *dst, size_t want)
                    {
                        if (offset >= at.size)
                            return size_t{0};
                        return s_parts[at.part].read(at.offset + offset, dst,
                                                     std::min<uint64_t>(want, at.size - offset));
                    }};
    }

    std::optional<std::vector<uint8_t>> readFile(size_t disc, const std::string &path)
    {
        if (disc >= s_discs.size())
            return std::nullopt;
        const auto &files = s_discs[disc].files;
        const auto it = files.find(key(path.c_str()));
        return read(it != files.end() ? &it->second : nullptr);
    }

    std::optional<size_t> discWithSerial(const std::string &serial)
    {
        for (size_t i = 0u; i < s_discs.size(); ++i)
        {
            if (s_discs[i].serial == serial)
                return i;
        }
        return std::nullopt;
    }

    void front(std::optional<size_t> layer) { s_front = layer; }

    void bootFrom(std::optional<size_t> layer, const std::vector<std::string> &paths)
    {
        s_boot = layer;
        s_bootFiles.clear();
        for (const std::string &path : paths)
            s_bootFiles.insert(key(path.c_str()));
    }

    std::optional<std::vector<uint8_t>> readFront(std::optional<size_t> layer, const std::string &path)
    {
        const std::string name = key(path.c_str());
        if (layer)
            if (const Entry *entry = findIn(s_layers[*layer], name))
                return read(entry);
        return readFile(0u, path);
    }

    std::optional<std::pair<uint32_t, uint32_t>> origin(const std::string &path)
    {
        const Entry *entry = find(key(path.c_str()));
        return entry ? std::optional(std::pair(entry->part, entry->offset)) : std::nullopt;
    }

    void rename(const std::string &as, size_t disc, const std::string &source)
    {
        s_renames.push_back({as, disc, source});
    }

    void install(PS2Runtime &runtime, const Addresses &addresses)
    {
        // Without a disc the game reads GEN/ beside the ELF, and its own
        // archive code stays.
        DiscImage *game = ps2ConfiguredDisc();
        if (!game)
            return;
        s_game = Volume::of(*game);
        if (!index(*s_game, "game disc"))
            return;
        for (size_t i = 0u; i < s_added.size(); ++i)
            index(*s_added[i], "disc " + std::to_string(i + 1u));

        s_blockSizeAddress = addresses.arkBlockSize;
        runtime.replaceFunction(addresses.archiveGetFileInfo, getFileInfo);
        runtime.replaceFunction(addresses.archiveIsValidBlock, isValidBlock);
        runtime.replaceFunction(addresses.cdRead, cdRead);
    }
}
