#pragma once

#include "addresses.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

class PS2Runtime;

namespace gh2::ark
{
    // A content disc, PS2 or 360, whose archive is searched after the game
    // disc's, in the order added. False, with the reason printed, if it
    // cannot be read.
    bool addDisc(const std::string &path);

    // A file served at `path`, ahead of every disc: a loose mod file, or
    // one built at startup.
    void addFile(const std::string &path, std::vector<uint8_t> bytes);

    // A file served at `path` ahead of every disc, made when the game first
    // asks for it: its size, and a reader of its bytes from an offset that
    // returns how many it gave. None if it cannot be made.
    struct Made
    {
        uint32_t size = 0u;
        std::function<size_t(uint64_t offset, uint8_t *dst, size_t size)> read;
    };
    void addMade(const std::string &path, std::function<std::optional<Made>()> make);

    // Every file under `root`, at its path relative to it. False, with the
    // reason printed, if it cannot be read.
    bool addFolder(const std::string &root);

    // A file's bytes as the game would get them, from wherever it lives.
    std::optional<std::vector<uint8_t>> readFile(const std::string &path);

    // A file's bytes from that disc's archive alone.
    std::optional<std::vector<uint8_t>> readFile(size_t disc, const std::string &path);

    // Which indexed disc boots that executable ("SLUS_215.86", or a 360
    // title ID, "415607E7"), once install has run.
    std::optional<size_t> discWithSerial(const std::string &serial);

    // Paths starting `as` are found as `source` plus the rest, on that disc
    // alone: "char/punk3/anims/" -> 80s "char/punk1/anims/".
    void rename(const std::string &as, size_t disc, const std::string &source);

    void install(PS2Runtime &runtime, const Addresses &addresses);
}
