#include "save/store.h"

#include "settings/ini.h"

#include <miniz.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>
#include <sstream>

namespace gh2::save
{
    namespace
    {
        // "GHCS", the format, then the text's size and its zlib stream.
        constexpr char kMagic[4] = {'G', 'H', 'C', 'S'};
        constexpr uint32_t kFormat = 1u;
        constexpr size_t kHeader = 12u;
        constexpr uint32_t kMaxText = 64u << 20;

        uint32_t readU32(const char *p)
        {
            uint32_t v;
            std::memcpy(&v, p, 4);
            return v;
        }
    }

    const std::string *Section::get(const std::string &key) const
    {
        for (const auto &[k, v] : values)
            if (k == key)
                return &v;
        return nullptr;
    }

    void Section::set(const std::string &key, const std::string &value)
    {
        for (auto &[k, v] : values)
            if (k == key)
            {
                v = value;
                return;
            }
        values.emplace_back(key, value);
    }

    const Section *Store::find(const std::string &name) const
    {
        for (const Section &s : sections)
            if (s.name == name)
                return &s;
        return nullptr;
    }

    Section &Store::section(const std::string &name)
    {
        for (Section &s : sections)
            if (s.name == name)
                return s;
        return sections.emplace_back(Section{name, {}});
    }

    std::string Store::text() const
    {
        std::string out;
        for (const Section &s : sections)
        {
            if (!out.empty())
                out += "\n";
            out += "[" + s.name + "]\n";
            for (const auto &[k, v] : s.values)
                out += k + " = " + v + "\n";
        }
        return out;
    }

    Store Store::parse(const std::string &text)
    {
        Store store;
        Section *current = nullptr;
        std::istringstream in(text);
        std::string line;
        while (std::getline(in, line))
        {
            line = ini::trim(line);
            if (ini::skipped(line))
                continue;
            if (const auto name = ini::sectionName(line))
            {
                current = &store.section(*name);
                continue;
            }
            const size_t eq = line.find('=');
            if (eq == std::string::npos || !current)
                continue;
            current->set(ini::trim(line.substr(0, eq)), ini::trim(line.substr(eq + 1)));
        }
        return store;
    }

    ReadResult readFile(const std::string &path, Store &out)
    {
        std::ifstream in(path, std::ios::binary);
        if (!in)
            return ReadResult::kMissing;
        const std::string file((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        if (file.size() < kHeader || std::memcmp(file.data(), kMagic, 4) != 0 || readU32(file.data() + 4) != kFormat)
            return ReadResult::kCorrupt;
        const uint32_t size = readU32(file.data() + 8);
        if (size > kMaxText)
            return ReadResult::kCorrupt;
        std::string text(size, '\0');
        mz_ulong got = size;
        if (mz_uncompress(reinterpret_cast<unsigned char *>(text.data()), &got,
                          reinterpret_cast<const unsigned char *>(file.data() + kHeader),
                          static_cast<mz_ulong>(file.size() - kHeader)) != MZ_OK ||
            got != size)
            return ReadResult::kCorrupt;
        out = Store::parse(text);
        return ReadResult::kOk;
    }

    bool writeFile(const std::string &path, const Store &store)
    {
        const std::string text = store.text();
        mz_ulong packedSize = mz_compressBound(static_cast<mz_ulong>(text.size()));
        std::string packed(packedSize, '\0');
        if (mz_compress2(reinterpret_cast<unsigned char *>(packed.data()), &packedSize,
                         reinterpret_cast<const unsigned char *>(text.data()), static_cast<mz_ulong>(text.size()),
                         MZ_BEST_COMPRESSION) != MZ_OK)
            return false;
        packed.resize(packedSize);
        const uint32_t header[2] = {kFormat, static_cast<uint32_t>(text.size())};
        return !ini::writeReplacing(
            path,
            [&](std::ofstream &out) {
                out.write(kMagic, 4);
                out.write(reinterpret_cast<const char *>(header), sizeof header);
                out.write(packed.data(), static_cast<std::streamsize>(packed.size()));
            },
            std::ios::binary);
    }
}
