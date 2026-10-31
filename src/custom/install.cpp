#include "custom/install.h"

#include "content/locale.h"
#include "content/setlists.h"
#include "custom/command.h"
#include "custom/index.h"
#include "custom/scan_status.h"
#include "custom/song_files.h"
#include "custom/songs.h"
#include "custom/text.h"
#include "save/save.h"
#include "settings/ini.h"
#include "settings/settings.h"

#include <algorithm>
#include <iostream>
#include <map>
#include <memory>
#include <thread>

namespace gh2
{
    namespace
    {
        namespace fs = std::filesystem;
        using namespace custom;

        // Beside settings.ini: the executable's folder when portable, else
        // the user data directory, and a test run's own.
        fs::path ownFolder()
        {
            const fs::path dir = fs::path(settings::besideSettings("songs"));
            std::error_code error;
            fs::create_directories(dir, error);
            return dir;
        }

        // Ours, then settings.ini's, split by ';'.
        std::vector<fs::path> roots()
        {
            std::vector<fs::path> out{ownFolder()};
            for (const std::string &folder : split(settings::text(settings::kSongFolders), ';'))
                if (const std::string one = ini::trim(folder); !one.empty())
                    out.emplace_back(fromUtf8(one));
            return out;
        }

        // The catalog as the list's rows, made again when it changes.
        std::shared_ptr<const std::vector<setlists::LiveRow>> rows()
        {
            static Snapshot s_of;
            static std::shared_ptr<const std::vector<setlists::LiveRow>> s_rows;
            const Snapshot now = catalog().now();
            if (now != s_of || !s_rows)
            {
                auto made = std::make_shared<std::vector<setlists::LiveRow>>();
                made->reserve(now->size());
                for (const Listed &listed : *now)
                    made->push_back({listed.id, listed.header});
                s_of = now;
                s_rows = std::move(made);
            }
            return s_rows;
        }

    }

    void installCustomSongs()
    {
        installCommand();
        installSongFiles();
        const fs::path own = ownFolder(), index = own / "index.txt";
        const std::vector<fs::path> folders = roots();
        const bool showCopies = settings::get(settings::kShowDuplicates) != 0;
        catalog().chartFolders(folders);
        catalog().showCopies(showCopies);
        // As the last run left them.
        setInstrument(save::state("custom", "instrument") == "bass" ? Part::Bass : Part::Guitar);
        if (const auto sort = sortNamed(save::state("custom", "sort")))
            catalog().sortBy(*sort, instrumentName());

        // The last scan's songs, then the scan, behind the game: each song
        // listed as it is found, the list's window taking it the next time
        // it fills.
        std::thread([folders, index, own, showCopies]
                    {
                        const std::vector<Song> known = readIndex(index);
                        for (const Song &song : known)
                            catalog().put(song);
                        std::cerr << "[custom] " << known.size() << " songs from the last scan" << std::endl;
                        ScanStatus &status = scanStatus();
                        status.expected = known.size();
                        status.running = true;
                        const std::vector<Song> scanned = scan(folders, known, 32u, [&status](const Song &s)
                                                               {
                                                                   catalog().put(s);
                                                                   ++status.seen;
                                                               });
                        status.running = false;
                        catalog().keepOnly(scanned);
                        writeIndex(index, scanned);
                        const auto copied = showCopies ? std::map<fs::path, fs::path>() : copies(scanned);
                        writeBadSongs(own / "badsongs.txt", scanned);
                        writeSongList(own / "songs.csv", scanned, copied);
                        const auto plays = std::count_if(scanned.begin(), scanned.end(), [](const Song &s) { return s.why.empty(); });
                        std::cerr << "[custom] scanned " << scanned.size() << " songs, " << plays << " play, "
                                  << copied.size() << " of them copies; badsongs.txt has the rest"
                                  << std::endl;
                    })
            .detach();

        locale::add("setlist_custom", "CUSTOM");
        // Each row its song's artist under its name.
        const auto artist = [](const std::string &id)
        {
            const auto song = catalog().find(id);
            return song ? plain(tag(song->tags, "artist").value_or("")) : std::string();
        };
        // On GH2's own paper whichever game's menus are up, as it goes on
        // for as long as the list (content/setlist_paper.h).
        setlists::addLive("custom", "gh2/ui/sel_song_quickplay.milo", {rows, readyEntries, "small1", artist});
    }
}
