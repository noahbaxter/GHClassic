#include "custom/install.h"

#include "content/locale.h"
#include "content/setlists.h"
#include "custom/audio.h"
#include "custom/cast.h"
#include "custom/command.h"
#include "custom/index.h"
#include "custom/scan_status.h"
#include "custom/song_files.h"
#include "custom/songs.h"
#include "custom/stage/stage.h"
#include "custom/text.h"
#include "disc/ark.h"
#include "formats/dtb.h"
#include "save/results.h"
#include "save/save.h"
#include "settings/ini.h"
#include "settings/settings.h"

#include <algorithm>
#include <chrono>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_set>

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

        // A custom song as [best] keeps it (save/results.h): by its chart, so a
        // moved folder or a copy keeps the one best, and the part it plays.
        std::optional<save::NamedSong> bestName(const std::string &id)
        {
            const auto song = catalog().find(id);
            if (!song)
                return std::nullopt;
            return save::NamedSong{"custom." + song->chartMd5, kPartNames[static_cast<int>(played(*song))]};
        }

        // Each of GH2's hand maps, wherever its fret parsers list them, again
        // as kOpenHandMap's. A map's events match in order, a list of lanes
        // any gem holding them all (MidiParser, 0x23ed80), so an open, a gem
        // with no fret (gem_kinds.h), falls to its () with the chords. Here
        // () is finger_open, the chords it took matched first by their
        // lowest lane: ((3) clip) ((4) clip); a map with no () ends on it.
        void addOpenHandMaps(dtb::Node &node)
        {
            if (dtb::find(node, "HandMap_Default"))
            {
                for (size_t i = 1u, maps = node.nodes.size(); i < maps; ++i)
                    if (dtb::Node map = node.nodes[i]; map.nodes.size() >= 2u && map.nodes[0].text.starts_with("HandMap_"))
                        if (dtb::Node *events = const_cast<dtb::Node *>(dtb::find(map, "events")))
                        {
                            const auto chords = std::find_if(events->nodes.begin(), events->nodes.end(), [](const dtb::Node &e)
                                                             { return e.nodes.size() == 2u && e.nodes[0].type == dtb::kArray &&
                                                                      e.nodes[0].nodes.empty(); });
                            std::vector<dtb::Node> added;
                            if (chords != events->nodes.end())
                                for (const int lane : {3, 4})
                                    added.push_back(dtb::array({dtb::array({dtb::integer(lane)}), chords->nodes[1]}));
                            added.push_back(dtb::array({dtb::array({}), dtb::symbol("finger_open")}));
                            const auto at = chords != events->nodes.end() ? events->nodes.erase(chords) : events->nodes.end();
                            events->nodes.insert(at, added.begin(), added.end());
                            map.nodes[0].text += kOpenHandMap;
                            node.nodes.push_back(std::move(map));
                        }
                return;
            }
            for (dtb::Node &child : node.nodes)
                addOpenHandMaps(child);
        }
    }

    void installCustomSongs()
    {
        ark::addFix(0u, "config/gen/midi_parsers.dtb", [](std::vector<uint8_t> &file)
                    {
                        if (auto root = dtb::raw(file))
                        {
                            addOpenHandMaps(*root);
                            file = dtb::write(*root);
                        }
                    });
        installCommand();
        installSongFiles();
        save::nameSongs(bestName);
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
                        catalog().refresh();
                        std::cerr << "[custom] " << known.size() << " songs from the last scan" << std::endl;
                        ScanStatus &status = scanStatus();
                        status.expected = known.size();
                        status.running = true;
                        // The list made again every half second while the
                        // scan runs, here and not by a finder: one that
                        // waits on a slow folder leaves none of what came
                        // before it unlisted. Every half minute the index
                        // kept as it stands too, the last scan's songs not
                        // yet come to still in it, so a game quit before a
                        // long scan ends starts from what it found.
                        std::mutex foundMutex;
                        std::vector<Song> found;
                        std::thread ticker([&]
                                           {
                                               for (int tick = 1; status.running; ++tick)
                                               {
                                                   std::this_thread::sleep_for(std::chrono::milliseconds(500));
                                                   catalog().refresh();
                                                   if (tick % 60 != 0 || !status.running)
                                                       continue;
                                                   std::vector<Song> soFar;
                                                   {
                                                       const std::lock_guard lock(foundMutex);
                                                       soFar = found;
                                                   }
                                                   std::unordered_set<fs::path::string_type> seen;
                                                   for (const Song &s : soFar)
                                                       seen.insert(s.dir.native());
                                                   for (const Song &s : known)
                                                       if (!seen.count(s.dir.native()))
                                                           soFar.push_back(s);
                                                   writeIndex(index, soFar);
                                               }
                                           });
                        const std::vector<Song> scanned = scan(folders, known, 32u, [&](const Song &s)
                                                               {
                                                                   catalog().put(s);
                                                                   ++status.seen;
                                                                   const std::lock_guard lock(foundMutex);
                                                                   found.push_back(s);
                                                               }, waitForList);
                        status.running = false;
                        ticker.join();
                        catalog().keepOnly(scanned);
                        catalog().refresh();
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
        // Each song in its cast's venue (custom/cast.h).
        const auto venue = [](const std::string &id)
        {
            const auto song = catalog().find(id);
            return song ? castOf(song->tags).venue : std::string("small1");
        };
        // On GH2's own paper whichever game's menus are up, as it goes on
        // for as long as the list (content/setlist_paper.h).
        setlists::addLive("custom", "gh2/ui/sel_song_quickplay.milo", {rows, readyEntries, venue, artist});
    }
}
