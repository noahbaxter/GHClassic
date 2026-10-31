#pragma once

// A .sng package (github.com/mdsitton/SngFileFormat), one file holding a
// song folder: "SNGPKG", a version, a 16-byte mask, then song.ini's tags,
// an index of files (each a name, a length and where it starts in the
// package), and the files, each masked with byte i XORed by
// mask[i & 15] ^ (i & 0xff). Only the header and index are read to know
// it; a file is read when it is asked for.

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>

namespace gh2::sng
{
    struct Package
    {
        struct File
        {
            std::string name; // as the package spells it
            uint64_t at = 0u, size = 0u;
        };

        std::map<std::string, std::string> tags; // keys lowercased
        std::map<std::string, File> files;       // by name lowercased
        uint8_t mask[16] = {};
    };

    // Its header and index; none, with why, if it is not one this reads.
    std::optional<Package> read(const std::filesystem::path &path, std::string &why);

    // Its file named `name` (any case), unmasked, at most its first `most`
    // bytes; empty if it has none.
    std::string unpack(const std::filesystem::path &path, const Package &sng, const std::string &name,
                       size_t most = SIZE_MAX);
}
