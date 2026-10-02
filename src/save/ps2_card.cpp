#include "save/ps2_card.h"

#include "ini.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iterator>

namespace gh2::save
{
    namespace
    {
        constexpr uint32_t kPageData = 512;
        constexpr uint32_t kPageRaw = 528; // with 16 spare bytes
        constexpr uint32_t kPages = 16384;
        constexpr uint32_t kClusterData = 2 * kPageData;
        constexpr uint32_t kEntrySize = 512;
        constexpr uint32_t kEnd = 0xffffffffu;   // a chain's last cluster, or a file with none
        constexpr uint32_t kInUse = 0x80000000u; // a FAT entry's in-use bit
        constexpr char kMagic[] = "Sony PS2 Memory Card Format ";

        // Superblock fields (MemoryCardFolder.h, struct superblock).
        constexpr uint32_t kAllocOffset = 0x34;
        constexpr uint32_t kAllocEnd = 0x38;
        constexpr uint32_t kRootCluster = 0x3c;
        constexpr uint32_t kIfcList = 0x50;

        // Directory entry fields and modes (mcman-internal.h, McFsEntry).
        constexpr uint32_t kMode = 0x00, kLength = 0x04, kCreated = 0x08, kCluster = 0x10, kDirEntry = 0x14,
                           kModified = 0x18, kName = 0x40;
        constexpr uint16_t kExists = 0x8000;
        constexpr uint16_t kDirMode = 0x8427;  // rwx, directory, 0x400, exists
        constexpr uint16_t kFileMode = 0x8497; // rwx, file, closed, 0x400, exists
        constexpr uint16_t kRootParentMode = 0xa426;

        uint32_t u32At(const uint8_t *p)
        {
            uint32_t v;
            std::memcpy(&v, p, 4);
            return v;
        }

        void putU32(uint8_t *p, uint32_t v)
        {
            std::memcpy(p, &v, 4);
        }

        int parity(uint8_t b)
        {
            b ^= b >> 4;
            b ^= b >> 2;
            b ^= b >> 1;
            return b & 1;
        }

        // Three bytes of Hamming code per 128 bytes (MemoryCardFolder.cpp
        // CalculateECC, mcman McDataChecksum).
        void ecc(const uint8_t *data, uint8_t *out)
        {
            static const std::array<uint8_t, 256> kColumn = [] {
                const uint8_t masks[7] = {0x55, 0x33, 0x0f, 0x00, 0xaa, 0xcc, 0xf0};
                std::array<uint8_t, 256> t{};
                for (int b = 0; b < 256; ++b)
                    for (int k = 0; k < 7; ++k)
                        t[b] |= static_cast<uint8_t>(parity(static_cast<uint8_t>(b & masks[k])) << k);
                return t;
            }();
            uint8_t column = 0x77, line0 = 0x7f, line1 = 0x7f;
            for (int i = 0; i < 128; ++i)
            {
                column ^= kColumn[data[i]];
                if (parity(data[i]))
                {
                    line0 ^= static_cast<uint8_t>(~i);
                    line1 ^= static_cast<uint8_t>(i);
                }
            }
            out[0] = column;
            out[1] = line0 & 0x7f;
            out[2] = line1 & 0x7f;
        }

        // The card's clock runs on Japan time (mymc ps2mc_dir.py).
        void stamp(uint8_t *tod)
        {
            const std::time_t now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now()) + 9 * 3600;
            std::tm t{};
#ifdef _WIN32
            gmtime_s(&t, &now);
#else
            gmtime_r(&now, &t);
#endif
            const uint16_t year = static_cast<uint16_t>(t.tm_year + 1900);
            tod[0] = 0;
            tod[1] = static_cast<uint8_t>(t.tm_sec);
            tod[2] = static_cast<uint8_t>(t.tm_min);
            tod[3] = static_cast<uint8_t>(t.tm_hour);
            tod[4] = static_cast<uint8_t>(t.tm_mday);
            tod[5] = static_cast<uint8_t>(t.tm_mon + 1);
            std::memcpy(tod + 6, &year, 2);
        }

        std::vector<uint8_t> makeEntry(uint16_t mode, uint32_t length, uint32_t cluster, uint32_t dirEntry,
                                       const std::string &name)
        {
            std::vector<uint8_t> e(kEntrySize, 0);
            std::memcpy(e.data() + kMode, &mode, 2);
            putU32(e.data() + kLength, length);
            stamp(e.data() + kCreated);
            putU32(e.data() + kCluster, cluster);
            putU32(e.data() + kDirEntry, dirEntry);
            stamp(e.data() + kModified);
            std::memcpy(e.data() + kName, name.data(), std::min<size_t>(name.size(), 31));
            return e;
        }

        uint16_t modeOf(const std::vector<uint8_t> &e)
        {
            uint16_t m;
            std::memcpy(&m, e.data() + kMode, 2);
            return m;
        }

        std::string nameOf(const std::vector<uint8_t> &e)
        {
            const char *n = reinterpret_cast<const char *>(e.data() + kName);
            return std::string(n, std::find(n, n + 32, '\0'));
        }
    }

    std::optional<Ps2Card> Ps2Card::open(const std::string &path)
    {
        std::ifstream in(path, std::ios::binary);
        if (!in)
            return std::nullopt;
        Ps2Card card;
        card.m_raw.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        if (card.m_raw.size() != kPages * kPageRaw || std::memcmp(card.page(0), kMagic, 28) != 0)
            return std::nullopt;
        return card;
    }

    // Laid out as mymc and PCSX2 do for an 8 MB card: the superblock, the
    // indirect FAT at cluster 8, the FAT in 9 to 40, and the root directory
    // in the first data cluster, 41. Everything else stays erased.
    Ps2Card Ps2Card::format()
    {
        Ps2Card card;
        card.m_raw.assign(kPages * kPageRaw, 0xff);

        uint8_t super[kClusterData] = {};
        std::memcpy(super, kMagic, 28);
        std::memcpy(super + 0x1c, "1.2.0.0", 7);
        const uint16_t shorts[4] = {512, 2, 16, 0xff00}; // page_len, pages_per_cluster, pages_per_block, unused
        std::memcpy(super + 0x28, shorts, sizeof shorts);
        const uint32_t words[6] = {8192, 41, 8135, 0, 1023, 1022};
        std::memcpy(super + 0x30, words, sizeof words);
        putU32(super + kIfcList, 8);
        for (int i = 0; i < 32; ++i)
            putU32(super + 0xd0 + 4 * i, kEnd); // no bad blocks
        super[0x150] = 2;    // card_type: PS2
        super[0x151] = 0x2b; // card_flags, as PCSX2 reports them (MemoryCardProtocol.cpp)
        // Page 1 is PCSX2's: it keeps a checksum there (MemoryCardFile.cpp).
        std::memcpy(card.page(0), super, kPageData);
        ecc(super, card.page(0) + kPageData);
        for (int j = 1; j < 4; ++j)
            ecc(super + 128 * j, card.page(0) + kPageData + 3 * j);
        std::memset(card.page(0) + kPageData + 12, 0, 4);

        uint8_t cluster[kClusterData];
        for (uint32_t i = 0; i < 256; ++i)
            putU32(cluster + 4 * i, i < 32 ? 9 + i : kEnd);
        card.writeCluster(8, cluster);
        for (uint32_t f = 0; f < 32; ++f)
        {
            for (uint32_t i = 0; i < 256; ++i)
            {
                const uint32_t relative = f * 256 + i;
                putU32(cluster + 4 * i, relative == 0 ? kEnd : relative < 8135 ? 0x7fffffffu : kEnd);
            }
            card.writeCluster(9 + f, cluster);
        }

        std::memset(cluster, 0, sizeof cluster);
        const std::vector<uint8_t> self = makeEntry(kDirMode, 2, 0, 0, ".");
        std::vector<uint8_t> parent = makeEntry(kRootParentMode, 0, 0, 0, "..");
        std::memcpy(cluster, self.data(), kEntrySize);
        std::memcpy(cluster + kEntrySize, parent.data(), kEntrySize);
        card.writeCluster(41, cluster);
        return card;
    }

    uint8_t *Ps2Card::page(uint32_t n)
    {
        return m_raw.data() + static_cast<size_t>(n) * kPageRaw;
    }

    const uint8_t *Ps2Card::page(uint32_t n) const
    {
        return m_raw.data() + static_cast<size_t>(n) * kPageRaw;
    }

    void Ps2Card::readCluster(uint32_t absolute, uint8_t *out) const
    {
        std::memcpy(out, page(2 * absolute), kPageData);
        std::memcpy(out + kPageData, page(2 * absolute + 1), kPageData);
    }

    void Ps2Card::writeCluster(uint32_t absolute, const uint8_t *data)
    {
        for (uint32_t p = 0; p < 2; ++p)
        {
            uint8_t *dst = page(2 * absolute + p);
            std::memcpy(dst, data + p * kPageData, kPageData);
            for (int j = 0; j < 4; ++j)
                ecc(dst + 128 * j, dst + kPageData + 3 * j);
            std::memset(dst + kPageData + 12, 0, 4);
        }
    }

    uint32_t Ps2Card::superU32(uint32_t offset) const
    {
        return u32At(page(0) + offset);
    }

    uint32_t Ps2Card::allocOffset() const
    {
        return superU32(kAllocOffset);
    }

    uint32_t Ps2Card::fat(uint32_t relative) const
    {
        uint8_t cluster[kClusterData];
        readCluster(superU32(kIfcList + 4 * (relative / 65536)), cluster);
        readCluster(u32At(cluster + 4 * ((relative / 256) % 256)), cluster);
        return u32At(cluster + 4 * (relative % 256));
    }

    void Ps2Card::setFat(uint32_t relative, uint32_t value)
    {
        uint8_t cluster[kClusterData];
        readCluster(superU32(kIfcList + 4 * (relative / 65536)), cluster);
        const uint32_t fatCluster = u32At(cluster + 4 * ((relative / 256) % 256));
        readCluster(fatCluster, cluster);
        putU32(cluster + 4 * (relative % 256), value);
        writeCluster(fatCluster, cluster);
    }

    // The lowest free cluster below the BIOS's limit, 7999 on an 8 MB card
    // (MemoryCardFolder.cpp GetAmountDataClusters).
    std::optional<uint32_t> Ps2Card::allocate()
    {
        const uint32_t limit = superU32(kAllocEnd) / 1000 * 1000 - 1;
        for (uint32_t c = 0; c < limit; ++c)
            if (!(fat(c) & kInUse))
            {
                setFat(c, kEnd);
                return c;
            }
        return std::nullopt;
    }

    std::vector<uint32_t> Ps2Card::chain(uint32_t relative) const
    {
        std::vector<uint32_t> clusters;
        for (uint32_t c = relative; c != kEnd && clusters.size() < kPages / 2;)
        {
            clusters.push_back(c);
            const uint32_t next = fat(c);
            if (next == kEnd || !(next & kInUse))
                break;
            c = next & ~kInUse;
        }
        return clusters;
    }

    void Ps2Card::freeChain(uint32_t relative)
    {
        for (uint32_t c : chain(relative))
            setFat(c, fat(c) & ~kInUse);
    }

    std::vector<uint8_t> Ps2Card::readEntry(const Entry &at) const
    {
        uint8_t cluster[kClusterData];
        readCluster(allocOffset() + at.cluster, cluster);
        return std::vector<uint8_t>(cluster + at.slot * kEntrySize, cluster + (at.slot + 1) * kEntrySize);
    }

    void Ps2Card::writeEntry(const Entry &at, const std::vector<uint8_t> &entry)
    {
        uint8_t cluster[kClusterData];
        readCluster(allocOffset() + at.cluster, cluster);
        std::memcpy(cluster + at.slot * kEntrySize, entry.data(), kEntrySize);
        writeCluster(allocOffset() + at.cluster, cluster);
    }

    std::vector<Ps2Card::Entry> Ps2Card::entries(uint32_t first, uint32_t count) const
    {
        const std::vector<uint32_t> clusters = chain(first);
        std::vector<Entry> out;
        for (uint32_t i = 0; i < count && i / 2 < clusters.size(); ++i)
            out.push_back({clusters[i / 2], static_cast<int>(i % 2)});
        return out;
    }

    std::optional<Ps2Card::Entry> Ps2Card::findIn(uint32_t first, uint32_t count, const std::string &name) const
    {
        for (const Entry &at : entries(first, count))
        {
            const std::vector<uint8_t> e = readEntry(at);
            if ((modeOf(e) & kExists) && nameOf(e) == name)
                return at;
        }
        return std::nullopt;
    }

    std::vector<uint8_t> Ps2Card::fileData(const std::vector<uint8_t> &entry) const
    {
        const uint32_t length = u32At(entry.data() + kLength);
        std::vector<uint8_t> data;
        if (length == 0 || u32At(entry.data() + kCluster) == kEnd)
            return data;
        uint8_t cluster[kClusterData];
        for (uint32_t c : chain(u32At(entry.data() + kCluster)))
        {
            readCluster(allocOffset() + c, cluster);
            data.insert(data.end(), cluster, cluster + kClusterData);
            if (data.size() >= length)
                break;
        }
        data.resize(std::min<size_t>(data.size(), length));
        return data;
    }

    std::optional<std::vector<uint8_t>> Ps2Card::read(const std::string &dir, const std::string &file) const
    {
        const uint32_t root = superU32(kRootCluster);
        const uint32_t rootCount = u32At(readEntry({root, 0}).data() + kLength);
        const std::optional<Entry> d = findIn(root, rootCount, dir);
        if (!d)
            return std::nullopt;
        const std::vector<uint8_t> dirEntry = readEntry(*d);
        const std::optional<Entry> f =
            findIn(u32At(dirEntry.data() + kCluster), u32At(dirEntry.data() + kLength), file);
        if (!f)
            return std::nullopt;
        return fileData(readEntry(*f));
    }

    // Each entry loses its exists bit and its clusters their in-use bit; the
    // slots stay for reuse (mcman mcman_delete2).
    void Ps2Card::removeDir(const Entry &at)
    {
        std::vector<uint8_t> dirEntry = readEntry(at);
        const uint32_t first = u32At(dirEntry.data() + kCluster);
        for (const Entry &child : entries(first, u32At(dirEntry.data() + kLength)))
        {
            std::vector<uint8_t> e = readEntry(child);
            const std::string name = nameOf(e);
            if (!(modeOf(e) & kExists) || name == "." || name == "..")
                continue;
            if (u32At(e.data() + kCluster) != kEnd)
                freeChain(u32At(e.data() + kCluster));
            const uint16_t mode = modeOf(e) & ~kExists;
            std::memcpy(e.data() + kMode, &mode, 2);
            writeEntry(child, e);
        }
        freeChain(first);
        const uint16_t mode = modeOf(dirEntry) & ~kExists;
        std::memcpy(dirEntry.data() + kMode, &mode, 2);
        writeEntry(at, dirEntry);
    }

    bool Ps2Card::replaceDir(const std::string &dir, const std::vector<File> &files)
    {
        // On a copy, so a card with no room is left as it was.
        Ps2Card card = *this;
        const uint32_t root = superU32(kRootCluster);
        std::vector<uint8_t> rootSelf = card.readEntry({root, 0});
        uint32_t rootCount = u32At(rootSelf.data() + kLength);
        if (const std::optional<Entry> old = card.findIn(root, rootCount, dir))
            card.removeDir(*old);

        // A free slot past "." and "..", or a new one at the end.
        std::optional<Entry> slot;
        uint32_t index = 0;
        const std::vector<Entry> rootEntries = card.entries(root, rootCount);
        for (uint32_t i = 2; i < rootEntries.size() && !slot; ++i)
            if (!(modeOf(card.readEntry(rootEntries[i])) & kExists))
                slot = rootEntries[i], index = i;
        if (!slot)
        {
            index = rootCount;
            if (index % 2 == 0)
            {
                const std::optional<uint32_t> grown = card.allocate();
                if (!grown)
                    return false;
                const uint8_t blank[kClusterData] = {};
                card.writeCluster(card.allocOffset() + *grown, blank);
                card.setFat(card.chain(root).back(), *grown | kInUse);
                slot = Entry{*grown, 0};
            }
            else
            {
                slot = Entry{card.chain(root).back(), 1};
            }
            putU32(rootSelf.data() + kLength, ++rootCount);
            card.writeEntry({root, 0}, rootSelf);
        }

        // The directory's own entries: ".", "..", then the files.
        const uint32_t count = 2 + static_cast<uint32_t>(files.size());
        std::vector<uint32_t> dirClusters;
        for (uint32_t i = 0; i < (count + 1) / 2; ++i)
        {
            const std::optional<uint32_t> c = card.allocate();
            if (!c)
                return false;
            if (!dirClusters.empty())
                card.setFat(dirClusters.back(), *c | kInUse);
            dirClusters.push_back(*c);
        }
        std::vector<std::vector<uint8_t>> children = {makeEntry(kDirMode, 0, root, index, "."),
                                                      makeEntry(kDirMode, 0, 0, 0, "..")};
        for (const auto &[name, data] : files)
        {
            uint32_t first = kEnd, previous = kEnd;
            for (size_t at = 0; at < data.size(); at += kClusterData)
            {
                const std::optional<uint32_t> c = card.allocate();
                if (!c)
                    return false;
                uint8_t cluster[kClusterData] = {};
                std::memcpy(cluster, data.data() + at, std::min<size_t>(kClusterData, data.size() - at));
                card.writeCluster(card.allocOffset() + *c, cluster);
                if (previous != kEnd)
                    card.setFat(previous, *c | kInUse);
                else
                    first = *c;
                previous = *c;
            }
            children.push_back(makeEntry(kFileMode, static_cast<uint32_t>(data.size()), first, 0, name));
        }
        for (size_t i = 0; i < dirClusters.size(); ++i)
        {
            uint8_t cluster[kClusterData] = {};
            for (size_t s = 0; s < 2 && 2 * i + s < children.size(); ++s)
                std::memcpy(cluster + s * kEntrySize, children[2 * i + s].data(), kEntrySize);
            card.writeCluster(card.allocOffset() + dirClusters[i], cluster);
        }
        card.writeEntry(*slot, makeEntry(kDirMode, count, dirClusters.front(), 0, dir));
        *this = std::move(card);
        return true;
    }

    bool Ps2Card::save(const std::string &path) const
    {
        return !ini::writeReplacing(
            path,
            [&](std::ofstream &out) {
                out.write(reinterpret_cast<const char *>(m_raw.data()), static_cast<std::streamsize>(m_raw.size()));
            },
            std::ios::binary);
    }
}
