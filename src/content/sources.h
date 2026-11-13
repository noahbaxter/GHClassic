#pragma once

// A source is anything content comes from: a game's disc, a romhack, a
// folder of customs. Every song and venue of one goes by <source>_<name>
// everywhere past its files, GH2's own included ("gh2_surrender",
// "gh2_small1"), so no two sources can share a name and nothing is anyone's
// by default.
//
// Each is a row of one table: its kind, its source, its name on that
// source's disc, its id, and its own name, what its files and saves call
// it. A disc's files keep their names on the disc and are served under the
// ids (addDisc); inside them nothing changes. Where the game makes a name
// from an id to find something inside a file (a venue's drum kit and clip
// groups, its button on the venue map), it is made from the own name.

#include "addresses.h"
#include "formats/dtb.h"

#include <cstddef>
#include <deque>
#include <optional>
#include <string>
#include <vector>

class PS2Runtime;

namespace gh2::sources
{
    enum class Kind
    {
        Song,
        Venue,
    };

    struct Row
    {
        Kind kind;
        std::string source, name, id;
        // What the files and saves call it: its name, but for GH1's rooms
        // the GH2 venue each is built from (gh1/venues.h).
        std::string own;
    };

    // "gh2", "surrender" -> "gh2_surrender".
    inline std::string id(const std::string &source, const std::string &name)
    {
        return source + "_" + name;
    }

    // The name on its source's disc, when `ours` is that source's.
    inline std::optional<std::string> bare(const std::string &source, const std::string &ours)
    {
        const std::string prefix = source + "_";
        if (ours.size() <= prefix.size() || ours.compare(0u, prefix.size(), prefix) != 0)
            return std::nullopt;
        return ours.substr(prefix.size());
    }

    inline std::deque<Row> &table()
    {
        static std::deque<Row> s_rows;
        return s_rows;
    }

    // The row of that id, or none.
    inline const Row *find(const std::string &ours)
    {
        for (const Row &row : table())
            if (row.id == ours)
                return &row;
        return nullptr;
    }

    // A row, its own name the name unless given. The first for an id stays.
    inline const Row &add(Kind kind, const std::string &source, const std::string &name, const std::string &own = {})
    {
        if (const Row *row = find(id(source, name)))
            return *row;
        return table().emplace_back(Row{kind, source, name, id(source, name), own.empty() ? name : own});
    }

    // That kind's rows of that source, in the order added.
    inline std::vector<Row> rows(Kind kind, const std::string &source)
    {
        std::vector<Row> out;
        for (const Row &row : table())
            if (row.kind == kind && row.source == source)
                out.push_back(row);
        return out;
    }

    // What the files and saves call that id: its own name, the id if no row
    // has it.
    inline std::string own(const std::string &ours)
    {
        const Row *row = find(ours);
        return row ? row->own : ours;
    }

    // Locale keys made from a venue's id, beside the id itself: its setlist
    // header (the executable's song_header_%s), its address on the cash
    // award (endgame.dta's %s_address).
    inline constexpr const char *kVenueKeyPrefixes[] = {"song_header_"};
    inline constexpr const char *kVenueKeySuffixes[] = {"_address"};

    // The venue whose id a locale key is or is made from, or none.
    inline const Row *venueOfKey(const std::string &key)
    {
        const auto venue = [](const std::string &name) -> const Row *
        {
            const Row *row = find(name);
            return row && row->kind == Kind::Venue ? row : nullptr;
        };
        if (const Row *row = venue(key))
            return row;
        for (const std::string prefix : kVenueKeyPrefixes)
            if (key.starts_with(prefix))
                if (const Row *row = venue(key.substr(prefix.size())))
                    return row;
        for (const std::string suffix : kVenueKeySuffixes)
            if (key.size() > suffix.size() && key.ends_with(suffix))
                if (const Row *row = venue(key.substr(0u, key.size() - suffix.size())))
                    return row;
        return nullptr;
    }

    // That disc's songs (config/songs.dta, and `unlisted`, songs on it that
    // no entry names) and venues (world/<v>/, as its config lists them)
    // under that source's ids, in every script naming them. Once
    // ark::install has run, before anything reads its config.
    void addDisc(const std::string &source, size_t disc, const std::vector<std::string> &unlisted = {});

    // One songs.dta entry of that source's as ours: its name, and its paths
    // under songs/<name>/.
    void entry(dtb::Node &entry, const std::string &source);

    // {sources own <id>}, {sources owned <ids> <own>} (whether any of those
    // ids goes by that own name), and the venue made from GameConfig's
    // where a name is made from it (sources.cpp).
    void install(PS2Runtime &runtime, const Addresses &addresses);
}
