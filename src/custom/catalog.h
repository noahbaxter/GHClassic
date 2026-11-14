#pragma once

// The custom songs that play, as one list kept up to date while the game
// runs: filled from the index at the start, then each song the scan finds
// put in as it is found, from whichever thread found it, and those it did
// not find taken out once it is done. However many there are, the game
// lists a window of them at a time (content/setlists.h), from a snapshot
// that stays as it is while the list changes.

#include "custom/library.h"

#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace gh2::custom
{
    struct Listed
    {
        std::string id;     // its name in the game's config, cs_ and its folder's hash
        std::string header; // its tier's, by the catalog's sort: by folder, Catalog::chartFolders' section
        std::string name;   // title(*song)
        std::shared_ptr<const Song> song;
    };
    // In its sort's tiers, each by name.
    using Snapshot = std::shared_ptr<const std::vector<Listed>>;

    // What the list is in tiers by: the folder each song sits in, its
    // artist, its name's first letter, its charter, its genre, its length
    // to the minute, or song.ini's difficulty for the part played.
    enum class Sort
    {
        Folder,
        Artist,
        Title,
        Charter,
        Genre,
        Length,
        Difficulty,
    };
    inline constexpr const char *kSortNames[] = {"folder", "artist", "title", "charter", "genre", "length", "difficulty"};
    // The sort of that name, none for another.
    inline std::optional<Sort> sortNamed(const std::string &name)
    {
        for (size_t s = 0; s < std::size(kSortNames); ++s)
            if (name == kSortNames[s])
                return static_cast<Sort>(s);
        return std::nullopt;
    }

    class Catalog
    {
    public:
        // The folders scanned, before any song is put: a song's section by
        // folder is the folder it sits in and the one above ("Pack/Songs"),
        // under the deepest of these holding it, whose own name stands
        // alone ("Pack" for one straight in Pack).
        void chartFolders(const std::vector<std::filesystem::path> &roots);
        // A song found, or found changed; one that does not play is taken
        // out.
        void put(const Song &song);
        // Every song not among these taken out: a scan's whole list.
        void keepOnly(const std::vector<Song> &songs);
        // Each copy of a chart listed too, not only the first by folder.
        void showCopies(bool show);
        // The list in tiers by `sort`, a difficulty being `part`'s
        // (song.ini's diff_<part>).
        void sortBy(Sort sort, const std::string &part = "guitar");
        Sort sort() const;

        // The list as last made, made here only when there is none (at
        // first, or after a new sort): songs put in since wait for
        // refresh(), which the scan calls now and then, so the game thread
        // never sorts a big list while a scan runs.
        Snapshot now();
        // The list made again if songs have changed since it was.
        Snapshot refresh();
        std::shared_ptr<const Song> find(const std::string &id) const;

    private:
        mutable std::mutex m_mutex;
        // By folder as text: fs::path compares a part at a time.
        std::map<std::filesystem::path::string_type, Listed> m_byDir;
        std::map<std::string, std::filesystem::path::string_type> m_dirOf; // by id
        std::vector<std::filesystem::path> m_roots;
        Snapshot m_now;
        // Each change to the songs counted, and to their order (the sort,
        // copies shown): m_now is of the songs at m_nowVersion.
        uint64_t m_version = 0, m_nowVersion = 0, m_order = 0;
        bool m_showCopies = false;
        Sort m_sort = Sort::Folder;
        std::string m_part = "guitar";
    };

    // Its name as the list shows it: its song.ini's, else its folder's.
    std::string title(const Song &song);
    // A length in seconds as m:ss, to the nearest second.
    std::string lengthText(double seconds);
    // song.ini's diff_ for `part`, "guitar" or "bass"; guitar's is the
    // part played in its place, lead, else co-op, else rhythm. None if it
    // has none or not the part.
    std::optional<long> level(const Song &song, const std::string &part);
}
