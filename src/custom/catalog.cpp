#include "custom/catalog.h"

#include "custom/gh2_chart.h"
#include "custom/text.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <tuple>
#include <unordered_set>

namespace gh2::custom
{
    namespace
    {
        namespace fs = std::filesystem;

        // The same each start: cs_ and the folder's FNV-1a.
        std::string hashOf(const fs::path &dir)
        {
            uint32_t hash = 2166136261u;
            for (const unsigned char c : utf8(dir))
                hash = (hash ^ c) * 16777619u;
            char id[16];
            std::snprintf(id, sizeof id, "cs_%08x", hash);
            return id;
        }

        // A song's tier by `sort`: its rank among tiers (they sort by it
        // first, then by header) and its header. `folder` is the folder
        // it sits in, `name` its title().
        std::pair<int, std::string> tierOf(const Song &song, const std::string &folder, const std::string &name,
                                           Sort sort, const std::string &part)
        {
            const auto text = [&](const char *key) { return plain(tag(song.tags, key).value_or("")); };
            switch (sort)
            {
            case Sort::Folder:
                return {0, folder};
            case Sort::Artist:
                if (const std::string artist = text("artist"); !artist.empty())
                    return {0, artist};
                return {1, "UNKNOWN ARTIST"};
            case Sort::Charter:
                if (const std::string charter = text("charter"); !charter.empty())
                    return {0, charter};
                return {1, "UNKNOWN CHARTER"};
            case Sort::Title:
            {
                // A letter's own tier, the rest before them.
                const unsigned char c = name.empty() ? '#' : static_cast<unsigned char>(name[0]);
                return std::isalpha(c) ? std::pair{1, std::string(1, static_cast<char>(std::toupper(c)))}
                                       : std::pair{0, std::string("#")};
            }
            case Sort::Length:
            {
                constexpr int kLongest = 10; // minutes, the last tier and up
                const int minutes = std::min(static_cast<int>(song.length / 60.0), kLongest);
                return {minutes, minutes == kLongest ? std::to_string(kLongest) + ":00+"
                                                     : std::to_string(minutes) + ":00 - " +
                                                           std::to_string(minutes + 1) + ":00"};
            }
            case Sort::Difficulty:
                if (const auto at = level(song, part))
                    return {static_cast<int>(*at), "DIFFICULTY " + std::to_string(*at)};
                return {1000, "NO DIFFICULTY"};
            }
            return {0, folder};
        }

        // A folder's name as text: plain, with the apostrophes that chart
        // sites write as "_" in file names put back (It_s, We_ll, Don_t).
        std::string folderText(const fs::path &name)
        {
            std::string text = plain(utf8(name));
            const auto letter = [&](size_t i) { return i < text.size() && std::isalpha(static_cast<unsigned char>(text[i])); };
            const auto follows = [&](size_t at, const char *tail)
            {
                for (; *tail; ++tail, ++at)
                    if (at >= text.size() || std::tolower(static_cast<unsigned char>(text[at])) != *tail)
                        return false;
                return !letter(at);
            };
            for (size_t i = 1; i + 1 < text.size(); ++i)
                if (text[i] == '_' && letter(i - 1))
                    for (const char *tail : {"s", "t", "d", "m", "ll", "re", "ve"})
                        if (follows(i + 1, tail))
                        {
                            text[i] = '\'';
                            break;
                        }
            return text;
        }

        // A song's section by folder (Catalog::chartFolders); with no root
        // holding it, the folder it sits in.
        std::string section(const fs::path &dir, const std::vector<fs::path> &roots)
        {
            const fs::path in = dir.parent_path();
            std::vector<fs::path> folders = {in.filename()};
            bool held = false;
            for (const fs::path &root : roots)
            {
                const fs::path under = in.lexically_relative(root);
                if (under.empty() || *under.begin() == "..")
                    continue;
                std::vector<fs::path> those = {root.filename()};
                if (under != ".")
                    those.assign(under.begin(), under.end());
                if (!held || those.size() < folders.size())
                    folders = std::move(those);
                held = true;
            }
            std::string out;
            for (size_t i = folders.size() > 2u ? folders.size() - 2u : 0u; i < folders.size(); ++i)
                out += (out.empty() ? "" : "/") + folderText(folders[i]);
            return out;
        }
    }

    std::string title(const Song &song)
    {
        // A package's file name without its .sng.
        const fs::path &dir = song.dir;
        const std::string file = folderText(isPackage(dir) ? dir.stem() : dir.filename());
        const auto name = tag(song.tags, "name");
        if (!name)
            return file;
        // A name in a script the fonts can't draw (all "?") gives way to
        // its folder's, where that has letters.
        const auto lettered = [](const std::string &s)
        { return std::any_of(s.begin(), s.end(), [](unsigned char c) { return std::isalnum(c) || c >= 0xC0; }); };
        const std::string shown = plain(*name);
        return lettered(shown) || !lettered(file) ? shown : file;
    }

    std::string lengthText(double seconds)
    {
        const int whole = static_cast<int>(seconds + 0.5);
        char text[16];
        std::snprintf(text, sizeof text, "%d:%02d", whole / 60, whole % 60);
        return text;
    }

    std::optional<long> level(const Song &song, const std::string &part)
    {
        static constexpr const char *kKeys[] = {"guitar", "guitar_coop", "rhythm", "bass"};
        const bool bass = part == "bass";
        const uint8_t guitars = song.parts & ~(1u << int(Part::Bass));
        if (bass ? !hasPart(song.parts, Part::Bass) : guitars == 0u)
            return std::nullopt;
        const auto at = number(song.tags, std::string("diff_") + kKeys[int(bass ? Part::Bass : *leadPart(guitars))]);
        return at && *at >= 0 ? at : std::nullopt;
    }

    void Catalog::chartFolders(const std::vector<fs::path> &roots)
    {
        const std::lock_guard lock(m_mutex);
        m_roots.clear();
        // As the scan walks them, a trailing separator off.
        for (const fs::path &root : roots)
        {
            const fs::path normal = root.lexically_normal();
            m_roots.push_back(normal.has_filename() ? normal : normal.parent_path());
        }
    }

    void Catalog::put(const Song &song)
    {
        const std::lock_guard lock(m_mutex);
        const auto it = m_byDir.find(song.dir.native());
        if (!song.why.empty())
        {
            if (it != m_byDir.end())
            {
                m_dirOf.erase(it->second.id);
                m_byDir.erase(it);
                m_now.reset();
            }
            return;
        }
        if (it != m_byDir.end())
        {
            // A rescan finds most as they were.
            const Song &was = *it->second.song;
            if (was.chart == song.chart && was.iniTime == song.iniTime && was.chartTime == song.chartTime &&
                was.stems == song.stems && was.art == song.art)
                return;
            it->second.song = std::make_shared<const Song>(song);
            it->second.name = title(song);
            m_now.reset();
            return;
        }
        // Another folder with the same hash takes an x on the end.
        std::string id = hashOf(song.dir);
        while (m_dirOf.count(id))
            id += "x";
        m_dirOf[id] = song.dir.native();
        m_byDir[song.dir.native()] = {id, section(song.dir, m_roots), title(song),
                             std::make_shared<const Song>(song)};
        m_now.reset();
    }

    void Catalog::keepOnly(const std::vector<Song> &songs)
    {
        std::unordered_set<fs::path::string_type> found;
        for (const Song &song : songs)
            if (song.why.empty())
                found.insert(song.dir.native());
        const std::lock_guard lock(m_mutex);
        for (auto it = m_byDir.begin(); it != m_byDir.end();)
            if (!found.count(it->first))
            {
                m_dirOf.erase(it->second.id);
                it = m_byDir.erase(it);
                m_now.reset();
            }
            else
                ++it;
    }

    void Catalog::showCopies(bool show)
    {
        const std::lock_guard lock(m_mutex);
        if (show != m_showCopies)
            m_now.reset();
        m_showCopies = show;
    }

    Snapshot Catalog::now()
    {
        const std::lock_guard lock(m_mutex);
        if (m_now)
            return m_now;
        // Of copies of one chart, one listed unless all are; the others
        // stay, to be listed when it goes. By folder every folder lists its
        // copy, so a folder always shows everything in it.
        std::map<std::string, const Listed *> first;
        auto list = std::make_shared<std::vector<Listed>>();
        if (m_showCopies)
            for (const auto &[dir, listed] : m_byDir)
                list->push_back(listed);
        else
        {
            for (const auto &[dir, listed] : m_byDir)
            {
                const std::string key = m_sort == Sort::Folder ? listed.song->chartMd5 + '\n' + lower(listed.header)
                                                               : listed.song->chartMd5;
                if (const Listed *&kept = first[key]; !kept || listedBefore(*listed.song, *kept->song))
                    kept = &listed;
            }
            list->reserve(first.size());
            for (const auto &[hash, listed] : first)
                list->push_back(*listed);
        }
        // Each song's tier, sorted by its rank, then its header and name
        // whatever their case.
        struct Keyed
        {
            int rank;
            std::string tier, name;
            Listed listed;
        };
        std::vector<Keyed> keyed;
        keyed.reserve(list->size());
        for (Listed &listed : *list)
        {
            auto [rank, header] = tierOf(*listed.song, listed.header, listed.name, m_sort, m_part);
            listed.header = std::move(header);
            // A band by the word after its "The": The Dillinger Escape
            // Plan under D.
            std::string tier = lower(listed.header);
            if (m_sort == Sort::Artist && tier.rfind("the ", 0) == 0)
                tier.erase(0, 4);
            keyed.push_back({rank, std::move(tier), lower(listed.name), std::move(listed)});
        }
        std::stable_sort(keyed.begin(), keyed.end(), [](const Keyed &a, const Keyed &b)
                         { return std::tie(a.rank, a.tier, a.name) < std::tie(b.rank, b.tier, b.name); });
        // A tier spelled two ways (Metallica, METALLICA) is one, as its
        // first song spells it.
        list->clear();
        for (size_t i = 0; i < keyed.size(); ++i)
        {
            if (i > 0u && keyed[i].rank == keyed[i - 1u].rank && keyed[i].tier == keyed[i - 1u].tier)
                keyed[i].listed.header = list->back().header;
            list->push_back(std::move(keyed[i].listed));
        }
        m_now = std::move(list);
        return m_now;
    }

    void Catalog::sortBy(Sort sort, const std::string &part)
    {
        const std::lock_guard lock(m_mutex);
        if (sort != m_sort || part != m_part)
            m_now.reset();
        m_sort = sort;
        m_part = part;
    }

    Sort Catalog::sort() const
    {
        const std::lock_guard lock(m_mutex);
        return m_sort;
    }

    std::shared_ptr<const Song> Catalog::find(const std::string &id) const
    {
        const std::lock_guard lock(m_mutex);
        const auto dir = m_dirOf.find(id);
        return dir != m_dirOf.end() ? m_byDir.at(dir->second).song : nullptr;
    }
}
