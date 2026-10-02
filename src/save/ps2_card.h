#pragma once

// An 8 MB PS2 memory card image as PCSX2 keeps it (.ps2): 16384 pages of 512
// bytes, each followed by 16 spare bytes of ECC. Enough of the card's
// filesystem to read a save directory's files and to replace one whole.
//
// Sources: PCSX2's pcsx2/SIO/Memcard (MemoryCardFile.cpp, MemoryCardFolder.h),
// ps2sdk's mcman (main.c, ps2mc_fio.c) and mymc (ps2mc.py, ps2mc_dir.py).

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace gh2::save
{
    class Ps2Card
    {
    public:
        using File = std::pair<std::string, std::vector<uint8_t>>;

        // Nothing when the file is missing or not a formatted 8 MB card.
        static std::optional<Ps2Card> open(const std::string &path);
        // A blank card, formatted as PCSX2's BIOS would.
        static Ps2Card format();

        // "<directory>/<file>"'s bytes, if it is on the card.
        std::optional<std::vector<uint8_t>> read(const std::string &dir, const std::string &file) const;
        // Removes the directory if present, then writes it holding files.
        // False when the card has no room.
        bool replaceDir(const std::string &dir, const std::vector<File> &files);
        // Beside the old file and renamed over it.
        bool save(const std::string &path) const;

    private:
        struct Entry
        {
            uint32_t cluster; // relative cluster holding the entry
            int slot;         // 0 or 1 within it
        };

        std::vector<uint8_t> m_raw;

        uint8_t *page(uint32_t n);
        const uint8_t *page(uint32_t n) const;
        void readCluster(uint32_t absolute, uint8_t *out) const;
        void writeCluster(uint32_t absolute, const uint8_t *data);

        uint32_t superU32(uint32_t offset) const;
        uint32_t allocOffset() const;
        uint32_t fat(uint32_t relative) const;
        void setFat(uint32_t relative, uint32_t value);
        std::optional<uint32_t> allocate();
        void freeChain(uint32_t relative);
        std::vector<uint32_t> chain(uint32_t relative) const;

        std::vector<uint8_t> readEntry(const Entry &at) const;
        void writeEntry(const Entry &at, const std::vector<uint8_t> &entry);
        // The directory's entries in order, by its first cluster and count.
        std::vector<Entry> entries(uint32_t first, uint32_t count) const;
        std::optional<Entry> findIn(uint32_t first, uint32_t count, const std::string &name) const;
        std::vector<uint8_t> fileData(const std::vector<uint8_t> &entry) const;
        void removeDir(const Entry &at);
    };
}
