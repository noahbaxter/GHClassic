#pragma once

// A source is anything content comes from: a game's disc, a romhack, a
// folder of customs. Every song, venue, character and outfit of one goes by
// <source>_<name> everywhere past its files, GH2's own included
// ("gh2_surrender", "gh2_small1", "gh2_punk1"), so no two sources can share
// a name and nothing is anyone's by default. A character is a person,
// whichever disc names them: the 80s' punk is GH2's, gh2_punk, its new
// outfit gh80s_punk1. The band (metal_singer) are outfits of no character.
//
// Each is a row of one table: its kind, its source, its name on that
// source's disc, its id, and its own name, what its files and saves call
// it. A disc's files keep their names on the disc and are served under the
// ids (addDisc); inside them nothing changes. Where the game makes a name
// from an id to find something inside a file (a venue's drum kit and clip
// groups, its button on the venue map, a character's portrait), it is made
// from the own name.

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
        Character,
        Outfit,
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

    // The row of that id, or none. A character and its one outfit can share
    // one (gh2_grim): the character's is the first.
    inline const Row *find(const std::string &ours)
    {
        for (const Row &row : table())
            if (row.id == ours)
                return &row;
        return nullptr;
    }

    // The row of that kind with that id, or none.
    inline const Row *find(const std::string &ours, Kind kind)
    {
        for (const Row &row : table())
            if (row.id == ours && row.kind == kind)
                return &row;
        return nullptr;
    }

    // A row, its own name the name unless given. The first of a kind for an
    // id stays.
    inline const Row &add(Kind kind, const std::string &source, const std::string &name, const std::string &own = {})
    {
        if (const Row *row = find(id(source, name), kind))
            return *row;
        return table().emplace_back(Row{kind, source, name, id(source, name), own.empty() ? name : own});
    }

    // A character of that source's: the one of that name another source
    // added first, else its own.
    inline const Row &addCharacter(const std::string &source, const std::string &name)
    {
        for (const Row &row : table())
            if (row.kind == Kind::Character && row.name == name)
                return row;
        return add(Kind::Character, source, name);
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

    // Locale keys made from an id, beside the id itself: a venue's setlist
    // header (the executable's song_header_%s) and its address on the cash
    // award (endgame.dta's %s_address), an outfit's name on the picker
    // (multiplayer.dta's <outfit>_outfit), a character's blurbs (career.dta's
    // <character>_blurb, sel_character.dta's _outfit_blurb).
    struct KeyForm
    {
        Kind kind;
        const char *prefix, *suffix;
    };
    inline constexpr KeyForm kKeyForms[] = {
        {Kind::Venue, "song_header_", ""}, {Kind::Venue, "", "_address"},     {Kind::Outfit, "", "_outfit"},
        {Kind::Character, "", "_blurb"},   {Kind::Character, "", "_outfit_blurb"},
    };

    // The `kind` named by a locale key, made from `name` by a form of that
    // kind's, or none. The name itself is any kind's.
    inline std::optional<std::string> nameOfKey(const std::string &key, Kind kind)
    {
        for (const KeyForm &form : kKeyForms)
        {
            const std::string prefix = form.prefix, suffix = form.suffix;
            if (form.kind == kind && key.size() > prefix.size() + suffix.size() && key.starts_with(prefix) &&
                key.ends_with(suffix))
                return key.substr(prefix.size(), key.size() - prefix.size() - suffix.size());
        }
        return std::nullopt;
    }

    // The venue or outfit whose id a locale key is or is made from, or none.
    inline const Row *rowOfKey(const std::string &key)
    {
        for (const Kind kind : {Kind::Venue, Kind::Outfit})
        {
            const Row *row = find(key, kind);
            if (!row)
                if (const auto name = nameOfKey(key, kind))
                    row = find(*name, kind);
            if (row)
                return row;
        }
        return nullptr;
    }

    // That disc's songs (config/songs.dta, and `unlisted`, songs on it that
    // no entry names), venues (world/<v>/, as its config lists them),
    // characters and outfits (config's (characters ...), and any
    // char/<o>/og/<o>.milo: the band) under that source's ids, in every
    // script naming them. Once
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
