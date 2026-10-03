#include "disc/volume.h"

#include "runtime/ps2_disc_image.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <limits>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace gh2
{
    namespace
    {
        std::string lower(std::string s)
        {
            std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
            return s;
        }

        class Ps2Volume final : public Volume
        {
        public:
            Ps2Volume(std::unique_ptr<DiscImage> owned, DiscImage &disc) : m_owned(std::move(owned)), m_disc(disc) {}

            std::optional<File> find(const std::string &path) override
            {
                DiscImage::Extent extent;
                if (!m_disc.find(path, extent))
                    return std::nullopt;
                return File{static_cast<uint64_t>(extent.lbn) * DiscImage::kSectorSize, extent.size};
            }

            size_t read(uint64_t offset, uint8_t *dst, size_t size) override
            {
                DiscImage::Extent from;
                from.lbn = static_cast<uint32_t>(offset / DiscImage::kSectorSize);
                from.size = std::numeric_limits<uint32_t>::max();
                return m_disc.readExtent(from, offset % DiscImage::kSectorSize, dst, size);
            }

            // The boot executable's name from SYSTEM.CNF's BOOT2 line.
            std::string serial() override
            {
                const auto cnf = find("SYSTEM.CNF");
                if (!cnf)
                    return {};
                std::string text(static_cast<size_t>(cnf->size), '\0');
                text.resize(read(cnf->offset, reinterpret_cast<uint8_t *>(text.data()), text.size()));
                const size_t boot = text.find("BOOT2");
                const size_t start = text.find('\\', boot);
                const size_t end = text.find(';', start);
                if (boot == std::string::npos || start == std::string::npos || end == std::string::npos)
                    return {};
                return text.substr(start + 1u, end - start - 1u);
            }

        private:
            std::unique_ptr<DiscImage> m_owned;
            DiscImage &m_disc;
        };

        // A host file read under a lock, as DiscImage reads.
        class HostFile
        {
        public:
            bool open(const std::filesystem::path &path)
            {
                m_in.open(path, std::ios::binary);
                return m_in.is_open();
            }

            size_t read(uint64_t offset, uint8_t *dst, size_t size)
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_in.clear();
                m_in.seekg(static_cast<std::streamoff>(offset));
                m_in.read(reinterpret_cast<char *>(dst), static_cast<std::streamsize>(size));
                return static_cast<size_t>(m_in.gcount());
            }

        private:
            std::ifstream m_in;
            std::mutex m_mutex;
        };

        // XDVDFS: a volume descriptor "MICROSOFT*XBOX*MEDIA" at sector 32
        // of the game partition, naming the root directory's sector and
        // size. A directory is a binary tree of entries: left and right
        // subtree offsets in words, sector, size, attributes, name length
        // and name, 0xff padding between sectors.
        class XboxVolume final : public Volume
        {
        public:
            static constexpr uint64_t kSector = 2048u;

            bool open(const std::filesystem::path &path, std::string &error)
            {
                if (!m_file.open(path))
                {
                    error = "cannot open";
                    return false;
                }
                // Where the game partition starts: XGD2, XGD3, XGD1, or a
                // bare partition.
                for (const uint64_t base : {0xfd90000ull, 0x2080000ull, 0x18300000ull, 0ull})
                {
                    std::array<uint8_t, 28> descriptor{};
                    if (m_file.read(base + 32u * kSector, descriptor.data(), descriptor.size()) != descriptor.size() ||
                        std::memcmp(descriptor.data(), "MICROSOFT*XBOX*MEDIA", 20u) != 0)
                        continue;
                    m_base = base;
                    uint32_t root = 0u, rootSize = 0u;
                    std::memcpy(&root, descriptor.data() + 20u, 4u);
                    std::memcpy(&rootSize, descriptor.data() + 24u, 4u);
                    list(root, rootSize, "");
                    return true;
                }
                error = "not an Xbox 360 disc";
                return false;
            }

            std::optional<File> find(const std::string &path) override
            {
                const auto it = m_files.find(lower(path));
                return it != m_files.end() ? std::optional<File>(it->second) : std::nullopt;
            }

            size_t read(uint64_t offset, uint8_t *dst, size_t size) override
            {
                return m_file.read(m_base + offset, dst, size);
            }

            // The title ID in default.xex's execution info (optional header
            // 0x00040006: media ID, version, base version, title ID), within
            // the headers that end where the image starts (word 8),
            // big-endian.
            std::string serial() override
            {
                const auto xex = find("default.xex");
                if (!xex)
                    return {};
                std::vector<uint8_t> head(0x18u);
                const auto be32 = [&](size_t at) -> uint32_t
                {
                    if (at + 4u > head.size())
                        return 0u;
                    return uint32_t(head[at]) << 24 | uint32_t(head[at + 1u]) << 16 | uint32_t(head[at + 2u]) << 8 |
                           head[at + 3u];
                };
                if (read(xex->offset, head.data(), head.size()) != head.size() ||
                    std::memcmp(head.data(), "XEX2", 4u) != 0)
                    return {};
                head.resize(static_cast<size_t>(std::min<uint64_t>(xex->size, be32(8u))));
                head.resize(read(xex->offset, head.data(), head.size()));
                const uint32_t count = be32(0x14u);
                for (uint32_t i = 0u; i < count; ++i)
                {
                    if (be32(0x18u + 8u * i) != 0x00040006u)
                        continue;
                    char id[9];
                    std::snprintf(id, sizeof id, "%08X", be32(be32(0x1cu + 8u * i) + 12u));
                    return id;
                }
                return {};
            }

        private:
            void list(uint32_t sector, uint32_t size, const std::string &prefix)
            {
                std::vector<uint8_t> dir(size);
                dir.resize(read(sector * kSector, dir.data(), dir.size()));
                std::function<void(size_t, int)> entry = [&](size_t word, int depth)
                {
                    const size_t at = word * 4u;
                    if (depth > 64 || at + 14u > dir.size())
                        return;
                    uint16_t left = 0u, right = 0u;
                    uint32_t start = 0u, bytes = 0u;
                    std::memcpy(&left, dir.data() + at, 2u);
                    std::memcpy(&right, dir.data() + at + 2u, 2u);
                    if (left == 0xffffu && right == 0xffffu)
                        return;
                    std::memcpy(&start, dir.data() + at + 4u, 4u);
                    std::memcpy(&bytes, dir.data() + at + 8u, 4u);
                    const uint8_t attributes = dir[at + 12u], length = dir[at + 13u];
                    if (at + 14u + length > dir.size())
                        return;
                    const std::string name = prefix + lower(std::string(reinterpret_cast<const char *>(dir.data() + at + 14u), length));
                    if (left)
                        entry(left, depth + 1);
                    if (attributes & 0x10u)
                    {
                        if (bytes)
                            list(start, bytes, name + "/");
                    }
                    else
                        m_files[name] = {start * kSector, bytes};
                    if (right)
                        entry(right, depth + 1);
                };
                entry(0u, 0);
            }

            HostFile m_file;
            uint64_t m_base = 0u;
            std::unordered_map<std::string, File> m_files;
        };
    }

    std::unique_ptr<Volume> Volume::open(const std::filesystem::path &path, std::string &error)
    {
        auto xbox = std::make_unique<XboxVolume>();
        std::string xboxError;
        if (xbox->open(path, xboxError))
            return xbox;
        std::unique_ptr<DiscImage> disc = DiscImage::open(path, error);
        if (!disc)
            return nullptr;
        DiscImage &ref = *disc;
        return std::make_unique<Ps2Volume>(std::move(disc), ref);
    }

    std::unique_ptr<Volume> Volume::of(DiscImage &disc)
    {
        return std::make_unique<Ps2Volume>(nullptr, disc);
    }
}
