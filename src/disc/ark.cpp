// The game's archive, owned by the host: every disc's MAIN.HDR indexed here,
// and the three functions that turn a path into sectors replaced, so files
// can come from more than one disc, each read in place.
//
// Retail resolves a path in Archive::GetFileInfo (0x2ac618) to an ark part
// and a byte offset in it, bounds blocks with Archive::IsValidBlock
// (0x2ac9e8), and reads them through CDRead (0x2ae9c0), which calls
// sceCdRead at that part's LBA. Here part numbers run on past the game
// disc's own, one per part of each added disc.

#include "disc/ark.h"

#include "guest.h"
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "runtime/ps2_disc_image.h"

#include <algorithm>
#include <cstring>
#include <iostream>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace gh2::ark
{
    namespace
    {
        struct Part
        {
            DiscImage *disc;
            DiscImage::Extent extent;
            uint32_t size; // from the header, what IsValidBlock bounds by
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
            std::string serial; // as SYSTEM.CNF boots it, "SLUS_214.47"
            std::unordered_map<std::string, Entry> files; // dir/name -> entry
        };

        // Paths under `as` are `source`'s on one disc, and nowhere else.
        struct Rename
        {
            std::string as;
            size_t disc;
            std::string source;
        };

        std::vector<std::unique_ptr<DiscImage>> s_added;
        std::vector<Part> s_parts;
        std::vector<Disc> s_discs; // searched in order, the game disc first
        std::vector<Rename> s_renames;
        uint32_t s_blockSizeAddress = 0u;

        // The boot executable's name from SYSTEM.CNF's BOOT2 line.
        std::string serialOf(DiscImage &disc)
        {
            DiscImage::Extent extent;
            if (!disc.find("SYSTEM.CNF", extent))
                return {};
            std::string text(extent.size, '\0');
            text.resize(disc.readExtent(extent, 0u, reinterpret_cast<uint8_t *>(text.data()), text.size()));
            const size_t boot = text.find("BOOT2");
            const size_t start = text.find('\\', boot);
            const size_t end = text.find(';', start);
            if (boot == std::string::npos || start == std::string::npos || end == std::string::npos)
                return {};
            return text.substr(start + 1u, end - start - 1u);
        }

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
        // and uncompressed size.
        bool index(DiscImage &disc, const std::string &label)
        {
            DiscImage::Extent extent;
            if (!disc.find("GEN/MAIN.HDR", extent))
            {
                std::cerr << "[ark] " << label << ": no GEN/MAIN.HDR" << std::endl;
                return false;
            }
            std::vector<uint8_t> hdr(extent.size);
            if (disc.readExtent(extent, 0u, hdr.data(), hdr.size()) != hdr.size())
            {
                std::cerr << "[ark] " << label << ": short read of MAIN.HDR" << std::endl;
                return false;
            }

            size_t at = 0u;
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
                Part part{&disc, {}, partSizes[i]};
                const std::string path = "GEN/MAIN_" + std::to_string(i) + ".ARK";
                if (!disc.find(path, part.extent))
                {
                    std::cerr << "[ark] " << label << ": no " << path << std::endl;
                    s_parts.resize(firstPart);
                    return false;
                }
                s_parts.push_back(part);
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
            const std::string serial = serialOf(disc);
            std::cerr << "[ark] " << label << " (" << serial << "): " << files.size() << " files in " << partCount
                      << " part" << (partCount == 1u ? "" : "s") << std::endl;
            s_discs.push_back({serial, std::move(files)});
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

        // Archive::GetFileInfo(this, path, &part, &offset, &size,
        // &uncompressedSize), true when found.
        const Entry *find(const std::string &name)
        {
            for (const Rename &rename : s_renames)
            {
                if (name.compare(0u, rename.as.size(), rename.as) != 0)
                    continue;
                const auto &files = s_discs[rename.disc].files;
                const auto it = files.find(rename.source + name.substr(rename.as.size()));
                return it != files.end() ? &it->second : nullptr;
            }
            for (const Disc &disc : s_discs)
            {
                const auto it = disc.files.find(name);
                if (it != disc.files.end())
                    return &it->second;
            }
            return nullptr;
        }

        void getFileInfo(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            const uint32_t path = GPR_U32(ctx, 5);
            const Entry *found = nullptr;
            if (path != 0u)
            {
                const std::string name = key(reinterpret_cast<const char *>(getMemPtr(rdram, path)));
                found = find(name);                // Retail asks only for files it has, so a miss is news.
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
            if (part < s_parts.size() && offset < s_parts[part].extent.size)
            {
                const Part &p = s_parts[part];
                // The last block runs past the part's end; the disc beyond
                // it is never used, so only the file's bytes are read.
                const size_t want = std::min<uint64_t>(bytes, p.extent.size - offset);
                failed = p.disc->readExtent(p.extent, offset, getMemPtr(rdram, GPR_U32(ctx, 7)), want) != want;
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
        std::unique_ptr<DiscImage> disc = DiscImage::open(path, error);
        if (!disc)
        {
            std::cerr << "[ark] " << path << ": " << error << std::endl;
            return false;
        }
        s_added.push_back(std::move(disc));
        return true;
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

    void rename(const std::string &as, size_t disc, const std::string &source)
    {
        s_renames.push_back({as, disc, source});    }

    void install(PS2Runtime &runtime, const Addresses &addresses)
    {
        // Without a disc the game reads GEN/ beside the ELF, and its own
        // archive code stays.
        DiscImage *game = ps2ConfiguredDisc();
        if (!game || !index(*game, "game disc"))
            return;
        for (size_t i = 0u; i < s_added.size(); ++i)
            index(*s_added[i], "disc " + std::to_string(i + 1u));

        s_blockSizeAddress = addresses.arkBlockSize;        runtime.replaceFunction(addresses.archiveGetFileInfo, getFileInfo);
        runtime.replaceFunction(addresses.archiveIsValidBlock, isValidBlock);
        runtime.replaceFunction(addresses.cdRead, cdRead);
    }
}
