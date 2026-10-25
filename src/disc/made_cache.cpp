#include "disc/made_cache.h"

#include "disc/ark.h"
#include "settings/settings.h"

#include <miniz.h>

#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <random>

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#elif defined(_WIN32)
#include <windows.h>
#endif

namespace gh2::made_cache
{
    namespace
    {
        namespace fs = std::filesystem;

        constexpr char kMagic[4] = {'G', 'H', 'C', 'M'};

        fs::path executable()
        {
#if defined(__APPLE__)
            uint32_t size = 0;
            _NSGetExecutablePath(nullptr, &size);
            std::string path(size, '\0');
            return _NSGetExecutablePath(path.data(), &size) == 0 ? fs::path(path.c_str()) : fs::path();
#elif defined(_WIN32)
            wchar_t path[32768];
            const DWORD n = GetModuleFileNameW(nullptr, path, DWORD(std::size(path)));
            return n ? fs::path(std::wstring(path, n)) : fs::path();
#else
            std::error_code error;
            return fs::read_symlink("/proc/self/exe", error);
#endif
        }

        // What a kept file was kept by: this build, as its executable's size
        // and time, and the serial of every disc mounted. Empty, keeping
        // nothing, when the executable cannot be found.
        const std::string &key()
        {
            static const std::string made = []
            {
                std::error_code error;
                const fs::path exe = executable();
                const auto size = fs::file_size(exe, error);
                if (error)
                    return std::string();
                const auto time = fs::last_write_time(exe, error);
                if (error)
                    return std::string();
                std::string out = std::to_string(size) + " " + std::to_string(static_cast<long long>(time.time_since_epoch().count()));
                for (size_t i = 0; i < ark::discs(); ++i)
                    out += " " + ark::serial(i);
                return out;
            }();
            return made;
        }

        fs::path fileOf(const std::string &name)
        {
            return fs::path(settings::userDataPath("cache")) / name;
        }

        uint64_t getU64(const uint8_t *p)
        {
            uint64_t v = 0;
            std::memcpy(&v, p, 8u);
            return v;
        }

        void putU64(std::vector<uint8_t> &out, uint64_t v)
        {
            const auto *p = reinterpret_cast<const uint8_t *>(&v);
            out.insert(out.end(), p, p + 8);
        }
    }

    // The file: the magic, the key's length and the key, the bytes' length,
    // then the bytes deflated.
    std::optional<std::vector<uint8_t>> read(const std::string &name)
    {
        const std::string &want = key();
        if (want.empty())
            return std::nullopt;
        std::ifstream in(fileOf(name), std::ios::binary);
        if (!in)
            return std::nullopt;
        const std::vector<uint8_t> file{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
        size_t at = sizeof kMagic;
        if (file.size() < at + 8u || std::memcmp(file.data(), kMagic, sizeof kMagic) != 0)
            return std::nullopt;
        const uint64_t keyBytes = getU64(file.data() + at);
        at += 8u;
        if (keyBytes != want.size() || file.size() < at + keyBytes + 8u ||
            std::memcmp(file.data() + at, want.data(), want.size()) != 0)
            return std::nullopt;
        at += keyBytes;
        std::vector<uint8_t> bytes(getU64(file.data() + at));
        at += 8u;
        mz_ulong got = static_cast<mz_ulong>(bytes.size());
        if (mz_uncompress(bytes.data(), &got, file.data() + at, static_cast<mz_ulong>(file.size() - at)) != MZ_OK ||
            got != bytes.size())
            return std::nullopt;
        return bytes;
    }

    void write(const std::string &name, const std::vector<uint8_t> &bytes)
    {
        const std::string &want = key();
        if (want.empty())
            return;
        std::vector<uint8_t> file(kMagic, kMagic + sizeof kMagic);
        putU64(file, want.size());
        file.insert(file.end(), want.begin(), want.end());
        putU64(file, bytes.size());
        const size_t head = file.size();
        mz_ulong packed = mz_compressBound(static_cast<mz_ulong>(bytes.size()));
        file.resize(head + packed);
        if (mz_compress2(file.data() + head, &packed, bytes.data(), static_cast<mz_ulong>(bytes.size()),
                         MZ_BEST_SPEED) != MZ_OK)
            return;
        file.resize(head + packed);
        // Written whole under a name of its own, then put in place: a run
        // reading it meanwhile, or another run keeping it too, never sees
        // half of one.
        const fs::path to = fileOf(name);
        std::error_code error;
        fs::create_directories(to.parent_path(), error);
        fs::path part = to;
        part += "." + std::to_string(std::random_device{}()) + ".part";
        {
            std::ofstream out(part, std::ios::binary | std::ios::trunc);
            out.write(reinterpret_cast<const char *>(file.data()), static_cast<std::streamsize>(file.size()));
            if (!out)
            {
                std::cerr << "[made_cache] cannot write " << part << std::endl;
                return;
            }
        }
        fs::rename(part, to, error);
        if (error)
        {
            std::cerr << "[made_cache] cannot keep " << to << ": " << error.message() << std::endl;
            fs::remove(part, error);
        }
    }
}
