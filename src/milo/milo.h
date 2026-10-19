#pragma once

// Milo scene files on the host: inflated, split into their objects, and
// written back. Enough to rebuild an outfit, not to understand every class.

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace gh2::milo
{
    using Bytes = std::vector<uint8_t>;

    // A directory: its header, entries and each object's body as stored.
    struct Dir
    {
        uint32_t version = 0;          // 10 is GH1's, 24 GH2's, 25 360 GH2's
        std::string className, name;   // v24's root, e.g. BandCharacter "punk"
        uint32_t tableCount = 0;       // v24 string table sizing hints
        uint32_t tableSize = 0;
        std::vector<std::pair<std::string, std::string>> entries; // class, name
        Bytes root;                    // v24's own directory object
        std::vector<Bytes> bodies;     // one per entry
    };

    // Words and length-prefixed strings as bodies keep them. A read past the
    // end gives 0 or "", and str leaves o at the end.
    uint32_t u32(const Bytes &b, size_t o);
    std::string str(const Bytes &b, size_t &o);
    void putU32(Bytes &out, uint32_t v);
    void putStr(Bytes &out, const std::string &s);

    // A .milo_ps2 / .rnd_ps2 file's blocks, joined.
    std::optional<Bytes> inflate(const Bytes &file);

    std::optional<Dir> parse(const Bytes &raw);

    // A scene file's objects, packed or not (GH1's .gh files are not).
    std::optional<Dir> read(const Bytes &file);

    // Where the object of that name is among a dir's entries.
    std::optional<size_t> find(const Dir &dir, const std::string &name);

    // One more object, its string table hints grown to fit its names.
    void add(Dir &dir, const std::string &className, const std::string &name, Bytes body);

    // Every string in the root and bodies that starts `from`, with `to` in
    // its place.
    void replacePrefix(Dir &dir, const std::string &from, const std::string &to);

    // The first string in the root and bodies ending `suffix`.
    std::optional<std::string> findSuffix(const Dir &dir, const std::string &suffix);

    // In one body: `from`, length-prefixed, as `to`. False if it has none.
    bool replaceStr(Bytes &body, const std::string &from, const std::string &to);
    // A list of names in a body (a Group's objects, a score display's
    // meshes): where its count is, the list holding `name`. None when no
    // such list holds it.
    std::optional<size_t> listHolding(const Bytes &body, const std::string &name);
    // `name` added to the list holding another, after or before it.
    bool addAfter(Bytes &body, const std::string &after, const std::string &name);
    bool addBefore(Bytes &body, const std::string &before, const std::string &name);
    // `name` taken out of the list holding it.
    bool remove(Bytes &body, const std::string &name);

    // A v24 file the game loads: uncompressed blocks, each whole objects.
    Bytes write(const Dir &dir);
}
