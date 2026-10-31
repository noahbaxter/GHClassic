#include "formats/sng.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <type_traits>

namespace gh2::sng
{
    namespace
    {
        constexpr uint32_t kVersion = 1u;
        // More than any song's tags and index; past it, it is not one.
        constexpr uint64_t kMostSection = 1u << 20;

        std::string lower(std::string s)
        {
            std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
            return s;
        }

        // Little-endian reads, false once the file runs out.
        struct Reader
        {
            std::ifstream in;

            template <typename T>
            bool read(T &value)
            {
                uint8_t bytes[sizeof(T)];
                if (!in.read(reinterpret_cast<char *>(bytes), sizeof(T)))
                    return false;
                std::make_unsigned_t<T> bits = 0u;
                for (size_t i = sizeof(T); i-- > 0;)
                    bits = static_cast<std::make_unsigned_t<T>>((bits << 8) | bytes[i]);
                value = static_cast<T>(bits);
                return true;
            }

            bool read(std::string &text, uint64_t size)
            {
                if (size > kMostSection)
                    return false;
                text.resize(size);
                return size == 0u || static_cast<bool>(in.read(text.data(), static_cast<std::streamsize>(size)));
            }
        };
    }

    std::optional<Package> read(const std::filesystem::path &path, std::string &why)
    {
        Reader r{std::ifstream(path, std::ios::binary)};
        std::string magic;
        uint32_t version = 0u;
        Package sng;
        if (!r.read(magic, 6u) || magic != "SNGPKG" || !r.read(version) ||
            !r.in.read(reinterpret_cast<char *>(sng.mask), sizeof sng.mask))
        {
            why = "not an .sng package";
            return std::nullopt;
        }
        if (version != kVersion)
        {
            why = ".sng version " + std::to_string(version) + ", not 1";
            return std::nullopt;
        }
        uint64_t length = 0u, count = 0u;
        if (!r.read(length) || !r.read(count) || length > kMostSection)
        {
            why = ".sng tags cut short";
            return std::nullopt;
        }
        for (uint64_t i = 0; i < count; ++i)
        {
            int32_t keyLength = 0, valueLength = 0;
            std::string key, value;
            if (!r.read(keyLength) || keyLength < 0 || !r.read(key, uint32_t(keyLength)) || !r.read(valueLength) ||
                valueLength < 0 || !r.read(value, uint32_t(valueLength)))
            {
                why = ".sng tags cut short";
                return std::nullopt;
            }
            // Keys lowercased, as song.ini's are.
            sng.tags[lower(key)] = value;
        }
        if (!r.read(length) || !r.read(count) || length > kMostSection)
        {
            why = ".sng index cut short";
            return std::nullopt;
        }
        std::error_code error;
        const uint64_t end = std::filesystem::file_size(path, error);
        for (uint64_t i = 0; i < count; ++i)
        {
            uint8_t nameLength = 0u;
            Package::File file;
            if (!r.read(nameLength) || !r.read(file.name, nameLength) || !r.read(file.size) || !r.read(file.at))
            {
                why = ".sng index cut short";
                return std::nullopt;
            }
            if (error || file.at > end || file.size > end - file.at)
            {
                why = ".sng file " + file.name + " runs past its end";
                return std::nullopt;
            }
            sng.files[lower(file.name)] = file;
        }
        return sng;
    }

    std::string unpack(const std::filesystem::path &path, const Package &sng, const std::string &name, size_t most)
    {
        const auto it = sng.files.find(lower(name));
        if (it == sng.files.end())
            return {};
        std::ifstream in(path, std::ios::binary);
        std::string bytes(static_cast<size_t>(std::min<uint64_t>(it->second.size, most)), '\0');
        if (!in.seekg(static_cast<std::streamoff>(it->second.at)) ||
            !in.read(bytes.data(), static_cast<std::streamsize>(bytes.size())))
            return {};
        // The mask runs from each file's own first byte.
        for (size_t i = 0; i < bytes.size(); ++i)
            bytes[i] = static_cast<char>(bytes[i] ^ sng.mask[i & 15u] ^ static_cast<uint8_t>(i & 0xffu));
        return bytes;
    }
}
