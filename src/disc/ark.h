#pragma once

#include "addresses.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <utility>
#include <string>
#include <vector>

class PS2Runtime;

namespace gh2::ark
{
    // A content disc, PS2 or 360, whose archive is searched after the game
    // disc's, in the order added. False, with the reason printed, if it
    // cannot be read.
    bool addDisc(const std::filesystem::path &path);

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
    // One that is those bytes, kept for as long as it is read.
    Made held(std::vector<uint8_t> bytes);

    // A file served in place of that disc's own copy of `path`, wherever its
    // copy would be (as a rename's source, under a layer over it, or in the
    // disc's turn) and nowhere else, made when the game first asks for it.
    // None leaves the disc's own. Once install has run.
    void addMade(size_t disc, const std::string &path, std::function<std::optional<Made>()> make);

    // That disc's archive paths starting `prefix`, in order, once install
    // has run.
    std::vector<std::string> paths(size_t disc, const std::string &prefix);

    // Every file under `root`, at its path relative to it. False, with the
    // reason printed, if it cannot be read.
    bool addFolder(const std::string &root);

    // A file's bytes as the game would get them, from wherever it lives.
    std::optional<std::vector<uint8_t>> readFile(const std::string &path);
    // Its first `most` bytes at most: a stream's header.
    std::optional<std::vector<uint8_t>> readStart(const std::string &path, size_t most);

    // A file's bytes from that disc's archive alone.
    std::optional<std::vector<uint8_t>> readFile(size_t disc, const std::string &path);

    // Which indexed disc boots that executable ("SLUS_215.86", or a 360
    // title ID, "415607E7"), once install has run.
    std::optional<size_t> discWithSerial(const std::string &serial);

    // Paths starting `as` are found as `source` plus the rest, on that disc
    // alone: "char/punk3/anims/" -> 80s "char/punk1/anims/".
    void rename(const std::string &as, size_t disc, const std::string &source);

    // A layer: files of its own over a disc's archive, or over none. The
    // one in front is searched before the game disc's, after the loose files
    // and the renames, which stay what they are whichever is in front
    // (content/campaigns.h).
    size_t addLayer(std::optional<size_t> disc);
    void addFile(size_t layer, const std::string &path, std::vector<uint8_t> bytes);
    // That layer's file at `path` is that disc's at `source`, read where it
    // is. False if the disc has none.
    bool lend(size_t layer, const std::string &path, size_t disc, const std::string &source);
    // The same for a file of that disc's own outside its archive, `source`
    // a path on the disc ("VIDEOS/INTRO.PSS"): the game reads movies from
    // the disc, not the archive. False if the disc has none.
    bool lendFromDisc(size_t layer, const std::string &path, size_t disc, const std::string &source);
    void front(std::optional<size_t> layer);
    // A path starting `as` is the rest of it as it is found with that layer
    // in front, or with none: one game's file whichever game is played
    // ("ghc/gh2/ui/sel_character.milo" is GH2's own).
    void alias(const std::string &as, std::optional<size_t> layer);
    // Until unlent, a path starting `prefix` is found as it is with that
    // layer in front, or with none: another game's venue for one song.
    void lend(const std::string &prefix, std::optional<size_t> layer);
    void unlend(const std::string &prefix);
    // Until cleared (no layer), these files are that layer's, where it has
    // them, ahead of every disc and the layer in front: a boot's own scenes,
    // the logos and the card check's, shown as the game the boot will open
    // as before its campaign is switched to (content/campaigns.cpp).
    void bootFrom(std::optional<size_t> layer, const std::vector<std::string> &paths);

    // A file's bytes as that layer in front gives them: its own, its
    // disc's, else the game disc's. With no layer, the game disc's.
    std::optional<std::vector<uint8_t>> readFront(std::optional<size_t> layer, const std::string &path);

    // The layer in front's own file at `path`, or its disc's, read in place
    // as it is asked for. None with no layer in front, or none there.
    std::optional<Made> openFront(const std::string &path);

    // Where a file's bytes live, its part and offset there, as it is found
    // now: one disc's copy differs from another's.
    std::optional<std::pair<uint32_t, uint32_t>> origin(const std::string &path);

    void install(PS2Runtime &runtime, const Addresses &addresses);
}
