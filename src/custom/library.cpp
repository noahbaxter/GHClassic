#include "custom/library.h"

#include "content/audio_kind.h"
#include "custom/text.h"
#include "formats/md5.h"
#include "formats/sng.h"

#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <thread>
#include <unordered_map>
#include <unordered_set>

namespace gh2::custom
{
    namespace
    {
        namespace fs = std::filesystem;

        // Clone Hero's stem names, each .ogg, .opus, .mp3, .wav or .aiff;
        // then YARG's explicit takes, laid over the rest as YARG plays them
        // uncensored. Its _clean takes, in their place when censored, are
        // never read.
        const char *const kStems[] = {
            "song",    "guitar",  "rhythm",  "bass",    "keys",    "vocals",  "vocals_1", "vocals_2",
            "drums",   "drums_1", "drums_2", "drums_3", "drums_4", "crowd",
            "vocals_explicit", "vocals_1_explicit", "vocals_2_explicit", "crowd_explicit",
        };

        int64_t written(const fs::path &file)
        {
            std::error_code error;
            const auto t = fs::last_write_time(file, error);
            return error ? 0 : static_cast<int64_t>(t.time_since_epoch().count());
        }

        std::string slurp(const fs::path &file, size_t most = SIZE_MAX)
        {
            std::ifstream in(file, std::ios::binary);
            std::string s;
            if (most == SIZE_MAX)
            {
                std::ostringstream all;
                all << in.rdbuf();
                return all.str();
            }
            s.resize(most);
            in.read(s.data(), static_cast<std::streamsize>(most));
            s.resize(static_cast<size_t>(in.gcount()));
            return s;
        }

        // A song that does not play, and why.
        Song rejected(const fs::path &dir, Reject reject, const std::string &why)
        {
            Song song;
            song.dir = dir;
            song.why = why;
            song.reject = reject;
            return song;
        }

        // A song's files, a folder's or a package's: their names lowercased,
        // to as they are spelled, and the first `most` bytes of one by
        // either.
        struct Source
        {
            std::map<std::string, std::string> names;
            std::function<std::string(const std::string &name, size_t most)> read;
        };

        // Of `names`, the first the song has, as it spells it; empty if none.
        std::string firstOf(const Source &source, std::initializer_list<std::string> names)
        {
            for (const std::string &name : names)
                if (const auto it = source.names.find(name); it != source.names.end())
                    return it->second;
            return {};
        }

        // Why a stem won't play, by its first bytes whatever it is named
        // (an .mp3 that is MPEG-4, say); empty if it will.
        std::string unplayable(const std::string &stem, const Source &source)
        {
            const std::string head = source.read(stem, 128u);
            std::string what;
            if (!audio_file::kind(reinterpret_cast<const uint8_t *>(head.data()), head.size(), &what).empty())
                return {};
            return stem + (head.empty() ? " can't be read" : " is " + what);
        }

        // A folder's files by name lowercased, as the folder spells them.
        using Listing = std::map<std::string, fs::path>;

        // Its subfolders, each with whether it is a link to one.
        struct Subdir
        {
            fs::path path;
            bool linked = false;
        };

        Listing list(const fs::path &dir, std::vector<Subdir> *subdirs = nullptr)
        {
            Listing files;
            std::error_code error;
            for (fs::directory_iterator it(dir, fs::directory_options::skip_permission_denied, error), end;
                 !error && it != end; it.increment(error))
            {
                // The type from the listing itself: no stat, which a
                // network drive answers a round trip at a time. One that
                // can't be told is left out, not the rest after it.
                std::error_code entry;
                if (it->is_directory(entry))
                {
                    if (subdirs)
                        subdirs->push_back({it->path(), it->is_symlink(entry)});
                }
                else if (!entry)
                    files[lower(utf8(it->path().filename()))] = it->path();
            }
            return files;
        }

        Source folder(const fs::path &dir, const Listing &files)
        {
            Source source;
            for (const auto &[name, path] : files)
                source.names[name] = utf8(path.filename());
            source.read = [dir, files](const std::string &name, size_t most)
            {
                const auto it = files.find(lower(name));
                return slurp(it != files.end() ? it->second : dir / fromUtf8(name), most);
            };
            return source;
        }

        Source package(const fs::path &path, std::shared_ptr<const sng::Package> held)
        {
            Source source;
            for (const auto &[name, file] : held->files)
                source.names[name] = file.name;
            source.read = [path, held](const std::string &name, size_t most) { return sng::unpack(path, *held, name, most); };
            return source;
        }

        // Its notes.mid, else its notes.chart, as named there; none if neither.
        std::string chartIn(const Source &source) { return firstOf(source, {"notes.mid", "notes.chart"}); }

        // Its stems, as named there, in kStems' order.
        std::vector<std::string> stemsIn(const Source &source)
        {
            std::vector<std::string> stems;
            for (const char *stem : kStems)
                if (std::string name = firstOf(source, {std::string(stem) + ".ogg", std::string(stem) + ".opus",
                                                        std::string(stem) + ".mp3", std::string(stem) + ".wav",
                                                        std::string(stem) + ".aiff"});
                    !name.empty())
                    stems.push_back(std::move(name));
            return stems;
        }

        std::string artIn(const Source &source) { return firstOf(source, {"album.png", "album.jpg", "album.jpeg"}); }

        // A song read from its files: `tags` its song.ini's, the times when
        // its song.ini and chart were written.
        Song readListed(const fs::path &dir, const std::string &chartName, const Source &source, Tags tags,
                        int64_t iniTime, int64_t chartTime)
        {
            Song song;
            song.dir = dir;
            song.chart = chartName;
            song.iniTime = iniTime;
            song.chartTime = chartTime;
            song.tags = std::move(tags);
            song.stems = stemsIn(source);
            song.art = artIn(source);

            std::string why;
            const std::string bytes = source.read(chartName, SIZE_MAX);
            song.chartMd5 = md5::hex(bytes);
            const std::optional<Chart> chart = parseSongChart(chartName, bytes, options(song.tags), why);
            // .chart's Offset where song.ini has no delay, as YARG reads it:
            // the same lead, in seconds.
            if (chart && chart->offset != 0.0 && number(song.tags, "delay").value_or(0) == 0)
                song.tags["delay"] = std::to_string(std::lround(chart->offset * 1000.0));
            if (chart)
            {
                for (const auto &[part, difficulties] : chart->parts)
                {
                    song.parts |= static_cast<uint8_t>(1u << static_cast<int>(part));
                    for (size_t d = 0; d < difficulties.size(); ++d)
                    {
                        const std::vector<Gem> &gems = difficulties[d].gems;
                        if (gems.empty())
                            continue;
                        song.charted |= static_cast<uint16_t>(1u << (4 * static_cast<int>(part) + static_cast<int>(d)));
                        song.length = std::max(song.length, chart->seconds(gems.back().tick + gems.back().longest()));
                    }
                }
                for (const auto &[tick, name] : chart->sections)
                    if (std::find(song.sections.begin(), song.sections.end(), name) == song.sections.end())
                        song.sections.push_back(name);
            }

            const auto turnDown = [&](Reject reject, std::string why)
            {
                song.reject = reject;
                song.why = std::move(why);
                return std::move(song);
            };
            if (!chart)
                return turnDown(Reject::BadChart, chartName + (why.empty() ? "" : ": " + why));
            if (song.parts == 0u)
                return turnDown(Reject::NoPart, "no 5-fret guitar or bass part");
            if (song.stems.empty())
                return turnDown(Reject::NoAudio, "no audio");
            // Each stem opened only for a song that otherwise plays: most of
            // a library's rejects are drum charts, and every open is a round
            // trip.
            for (const std::string &stem : song.stems)
                if (std::string bad = unplayable(stem, source); !bad.empty())
                    return turnDown(Reject::BadAudio, std::move(bad));
            return song;
        }

        // When a listed file was written; 0 for one not there.
        int64_t written(const Listing &files, const std::string &name)
        {
            const auto it = files.find(lower(name));
            return it != files.end() ? written(it->second) : 0;
        }

        Song readFolder(const fs::path &dir, const std::string &chartName, const Listing &files)
        {
            const Source source = folder(dir, files);
            return readListed(dir, chartName, source, parseIni(source.read("song.ini", SIZE_MAX)),
                              written(files, "song.ini"), written(files, chartName));
        }

        // A package's song, its tags in it and both times the package's.
        Song readPackage(const fs::path &path)
        {
            std::string why;
            auto read = sng::read(path, why);
            if (!read)
                return rejected(path, Reject::BadPackage, why);
            const auto held = std::make_shared<const sng::Package>(std::move(*read));
            const Source source = package(path, held);
            const std::string chart = chartIn(source);
            if (chart.empty())
                return rejected(path, Reject::NoChart, "no notes.mid or notes.chart in it");
            const int64_t time = written(path);
            return readListed(path, chart, source, held->tags, time, time);
        }
    }

    Song read(const fs::path &dir, const std::string &chartName)
    {
        return isPackage(dir) ? readPackage(dir) : readFolder(dir, chartName, list(dir));
    }

    bool isPackage(const fs::path &path) { return lower(utf8(path.extension())) == ".sng"; }

    fs::path songPath(const Song &song, const std::string &name) { return song.dir / fromUtf8(name); }

    std::string songFile(const fs::path &path)
    {
        const fs::path in = path.parent_path();
        if (!isPackage(in))
            return slurp(path);
        std::string why;
        const auto package = sng::read(in, why);
        return package ? sng::unpack(in, *package, utf8(path.filename())) : std::string();
    }

    // Folders taken from one queue by many workers at once: on a network
    // drive each listing and read waits on a round trip, so the walk is as
    // fast as how many are in flight. Each folder is listed once; a song
    // folder's listing finds its chart and stems too.
    std::vector<Song> scan(const std::vector<fs::path> &roots, const std::vector<Song> &known, unsigned workers,
                           const std::function<void(const Song &)> &each)
    {
        std::unordered_map<fs::path::string_type, const Song *> seen;
        for (const Song &s : known)
            seen[s.dir.native()] = &s;

        std::mutex mutex;
        std::condition_variable wake;
        // Folders already queued, so roots inside one another walk a folder once.
        std::unordered_set<fs::path::string_type> queued;
        std::deque<fs::path> queue;
        for (const fs::path &root : roots)
            if (queued.insert(root.lexically_normal().native()).second)
                queue.push_back(root.lexically_normal());
        size_t busy = 0u;
        std::vector<Song> out;
        // Where each linked folder walked leads, so a link back up the tree
        // or a second link to the same place is walked once.
        std::set<fs::path> linkedTo;

        // One folder: a song, an .sng package, or more folders to walk.
        const auto visit = [&](const fs::path &dir)
        {
            std::vector<Subdir> subdirs;
            const Listing files = list(dir, &subdirs);
            std::vector<Song> found;
            for (const auto &[name, path] : files)
                // Not macOS's "._" companion files, which sit beside every
                // file copied to a network share.
                if (isPackage(path) && utf8(path.filename()).rfind("._", 0u) != 0u)
                {
                    // Read again only when written since.
                    const auto old = seen.find(path.native());
                    if (old != seen.end() && old->second->iniTime != 0 && old->second->iniTime == written(path))
                        found.push_back(*old->second);
                    else
                        found.push_back(readPackage(path));
                }
            if (files.count("song.ini"))
            {
                const Source source = folder(dir, files);
                const std::string chart = chartIn(source);
                const auto old = seen.find(dir.native());
                // Kept while its song.ini and chart are as they were and its
                // audio and art are the same files; one turned down for its
                // audio (it has parts) is read again, as a stem may have been
                // put right under the same name.
                if (chart.empty())
                    found.push_back(rejected(dir, Reject::NoChart, "no notes.mid or notes.chart"));
                else if (old != seen.end() && (old->second->why.empty() || old->second->parts == 0u) &&
                         old->second->chart == chart && old->second->iniTime == written(files, "song.ini") &&
                         old->second->chartTime == written(files, chart) && old->second->stems == stemsIn(source) &&
                         old->second->art == artIn(source))
                    found.push_back(*old->second);
                else
                    found.push_back(readFolder(dir, chart, files));
            }
            if (each)
                for (const Song &song : found)
                    each(song);
            std::error_code error;
            const fs::path here = std::any_of(subdirs.begin(), subdirs.end(), [](const Subdir &s) { return s.linked; })
                                      ? fs::weakly_canonical(dir, error)
                                      : fs::path();
            std::lock_guard lock(mutex);
            for (Song &song : found)
                out.push_back(std::move(song));
            for (Subdir &sub : subdirs)
            {
                if (sub.linked)
                {
                    const fs::path to = fs::weakly_canonical(sub.path, error);
                    const auto [rel, end] = std::mismatch(to.begin(), to.end(), here.begin(), here.end());
                    if (error || rel == to.end() || !linkedTo.insert(to).second)
                        continue;
                }
                if (queued.insert(sub.path.native()).second)
                    queue.push_back(std::move(sub.path));
            }
            wake.notify_all();
        };

        std::vector<std::thread> threads;
        for (unsigned i = 0; i < std::max(workers, 1u); ++i)
            threads.emplace_back([&]
                                 {
                                     std::unique_lock lock(mutex);
                                     while (true)
                                     {
                                         wake.wait(lock, [&] { return !queue.empty() || busy == 0u; });
                                         if (queue.empty())
                                             return;
                                         fs::path dir = std::move(queue.front());
                                         queue.pop_front();
                                         ++busy;
                                         lock.unlock();
                                         visit(dir);
                                         lock.lock();
                                         --busy;
                                         wake.notify_all();
                                     }
                                 });
        for (std::thread &thread : threads)
            thread.join();
        // The same order each scan, whichever worker found what.
        std::sort(out.begin(), out.end(), listedBefore);
        return out;
    }

    bool partStem(const std::string &file, const std::string &part)
    {
        const std::string stem = lower(file.substr(0u, file.rfind('.')));
        return stem == part || stem.rfind(part + "_", 0u) == 0u;
    }

    // As text, as fs::path compares a part at a time, many times slower.
    bool listedBefore(const Song &a, const Song &b) { return a.dir.native() < b.dir.native(); }

    std::map<fs::path, fs::path> copies(const std::vector<Song> &songs)
    {
        std::map<std::string, const Song *> listed;
        for (const Song &s : songs)
            if (s.why.empty())
                if (const Song *&first = listed[s.chartMd5]; !first || listedBefore(s, *first))
                    first = &s;
        std::map<fs::path, fs::path> out;
        for (const Song &s : songs)
            if (s.why.empty() && listed.at(s.chartMd5) != &s)
                out[s.dir] = listed.at(s.chartMd5)->dir;
        return out;
    }
}
