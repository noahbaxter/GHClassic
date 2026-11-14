#include "custom/audio.h"

#include "content/audio_file.h"
#include "content/pcm_mix.h"
#include "custom/song_files.h"
#include "custom/text.h"

#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <fstream>
#include <iostream>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <thread>

namespace gh2::custom
{
    namespace
    {
        namespace fs = std::filesystem;

        using Bytes = std::shared_ptr<const std::vector<uint8_t>>;

        Bytes readFile(const fs::path &file)
        {
            const std::string read = songFile(file);
            return std::make_shared<const std::vector<uint8_t>>(read.begin(), read.end());
        }

        // A stem read in a chunk at a time as its decoder asks, so a preview
        // reads only what it plays: a network drive gives a song's stems at
        // a few MB a second.
        constexpr uint64_t kChunk = 64u << 10;
        struct Stem
        {
            uint64_t size = 0u;
            std::vector<Bytes> chunks;
            size_t after = SIZE_MAX; // the chunk after the last a decoder missed
        };
        using StemPtr = std::shared_ptr<Stem>;

        // The stems of the last few songs read, a song's by its folder or
        // package, most recent first.
        constexpr size_t kHeldSongs = 3u; // the one previewed, the one played, a spare
        struct Held
        {
            fs::path dir;
            std::map<fs::path, StemPtr> files;
            bool previewed = false; // its preview's chunks read
        };
        // Never destroyed: the reader outlives main's statics.
        std::mutex &s_mutex = *new std::mutex;
        std::condition_variable &s_wake = *new std::condition_variable;
        std::condition_variable &s_listRead = *new std::condition_variable; // listWaits gone false
        std::list<Held> &s_held = *new std::list<Held>;
        // The song stemsReady last asked about, and the art wantArt last
        // asked for in its order, each kept while it is wanted.
        std::shared_ptr<const Song> &s_song = *new std::shared_ptr<const Song>;
        std::vector<fs::path> &s_artWanted = *new std::vector<fs::path>;
        std::map<fs::path, Bytes> &s_art = *new std::map<fs::path, Bytes>;
        fs::path &s_onDir = *new fs::path; // the song the list is on, the first wantArt was given


        Held &heldFor(const fs::path &dir)
        {
            auto it = std::find_if(s_held.begin(), s_held.end(), [&](const Held &h) { return h.dir == dir; });
            if (it == s_held.end())
                s_held.push_front({dir, {}});
            else
                s_held.splice(s_held.begin(), s_held, it);
            if (s_held.size() > kHeldSongs)
                s_held.pop_back();
            return s_held.front();
        }

        Held *heldAt(const fs::path &dir)
        {
            auto it = std::find_if(s_held.begin(), s_held.end(), [&](const Held &h) { return h.dir == dir; });
            return it == s_held.end() ? nullptr : &*it;
        }

        // That stem, its size found the first time; a package's read whole,
        // as its files are packed.
        StemPtr stemFor(const fs::path &file)
        {
            {
                const std::lock_guard lock(s_mutex);
                if (Held *h = heldAt(file.parent_path()))
                    if (const auto it = h->files.find(file); it != h->files.end())
                        return it->second;
            }
            auto stem = std::make_shared<Stem>();
            if (isPackage(file.parent_path()))
            {
                const Bytes all = readFile(file);
                stem->size = all->size();
                for (uint64_t at = 0u; at < stem->size; at += kChunk)
                    stem->chunks.push_back(std::make_shared<const std::vector<uint8_t>>(
                        all->begin() + std::ptrdiff_t(at), all->begin() + std::ptrdiff_t(std::min(at + kChunk, stem->size))));
            }
            else
            {
                std::error_code error;
                stem->size = fs::file_size(file, error);
                if (error)
                    stem->size = 0u;
                stem->chunks.resize(static_cast<size_t>((stem->size + kChunk - 1u) / kChunk));
            }
            const std::lock_guard lock(s_mutex);
            const auto [it, added] = heldFor(file.parent_path()).files.emplace(file, stem);
            return it->second;
        }

        // Chunks [first, first + count) off the disk into the stem.
        void readChunks(const fs::path &file, Stem &stem, size_t first, size_t count)
        {
            std::ifstream in(file, std::ios::binary);
            in.seekg(static_cast<std::streamoff>(first * kChunk));
            for (size_t c = first; c < first + count; ++c)
            {
                std::vector<uint8_t> bytes(static_cast<size_t>(std::min(kChunk, stem.size - c * kChunk)));
                in.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
                bytes.resize(static_cast<size_t>(std::max<std::streamsize>(in.gcount(), 0)));
                const std::lock_guard lock(s_mutex);
                stem.chunks[c] = std::make_shared<const std::vector<uint8_t>>(std::move(bytes));
            }
        }

        // The stem's bytes for a decoder, each chunk not yet read read then.
        FileBytes stemBytes(const fs::path &file, StemPtr stem)
        {
            return {stem->size, [file, stem](uint64_t offset, uint8_t *dst, size_t n)
                    {
                        size_t done = 0u;
                        while (done < n && offset + done < stem->size)
                        {
                            const size_t c = static_cast<size_t>((offset + done) / kChunk);
                            Bytes chunk;
                            // A decoder reading straight on (an MP3's length
                            // found, say) read up to a MB an open; one
                            // seeking about, a chunk.
                            size_t run = 0u;
                            {
                                const std::lock_guard lock(s_mutex);
                                chunk = stem->chunks[c];
                                const size_t most = c == stem->after ? 16u : 1u;
                                while (!chunk && run < most && c + run < stem->chunks.size() && !stem->chunks[c + run])
                                    ++run;
                                if (!chunk)
                                    stem->after = c + run;
                            }
                            if (!chunk)
                            {
                                readChunks(file, *stem, c, run);
                                const std::lock_guard lock(s_mutex);
                                chunk = stem->chunks[c];
                            }
                            const size_t in = static_cast<size_t>(offset + done - c * kChunk);
                            if (in >= chunk->size())
                                break;
                            const size_t take = std::min(n - done, chunk->size() - in);
                            std::memcpy(dst + done, chunk->data() + in, take);
                            done += take;
                        }
                        return done;
                    }};
        }

        // Read whole first for a song played, which is never to wait on the
        // disk partway in.
        std::unique_ptr<vgs::Pcm> openStem(const fs::path &file, bool whole)
        {
            StemPtr stem = stemFor(file);
            if (stem->size == 0u)
            {
                std::cerr << "[custom] cannot read " << utf8(file) << std::endl;
                return nullptr;
            }
            for (size_t c = 0u; whole && c < stem->chunks.size(); ++c)
            {
                bool read = false;
                {
                    const std::lock_guard lock(s_mutex);
                    read = stem->chunks[c] != nullptr;
                }
                if (!read)
                    readChunks(file, *stem, c, stem->chunks.size() - c);
            }
            return audio_file::open(stemBytes(file, std::move(stem)), utf8(file));
        }

        struct Mix
        {
            std::string path;
            std::vector<std::pair<fs::path, uint32_t>> stems;
            double lead = 0.0, seconds = 0.0;
            fs::path dir; // the song's
        };

        // Opened again for the song started (song_files.h), so it is read
        // whole then even if a preview opened it first.
        std::unique_ptr<vgs::Pcm> mixOf(const Mix &mix)
        {
            const auto playing = playingSong();
            const bool whole = playing && playing->dir == mix.dir;            auto mixer = std::make_unique<vgs::Mixer>();
            for (const auto &[file, pair] : mix.stems)
                if (auto pcm = openStem(file, whole))
                    mixer->inputs.push_back({std::move(pcm), pair});
            // The mix at the first stem's rate, any other read to it.
            mixer->rate = mixer->inputs.empty() ? 44100u : mixer->inputs.front().pcm->rate;
            for (auto &in : mixer->inputs)
                if (in.pcm->rate != mixer->rate)
                    in.pcm = std::make_unique<vgs::Resampled>(std::move(in.pcm), mixer->rate);
            mixer->channels = 4u;
            mixer->leadFrames = static_cast<int64_t>(std::llround(mix.lead * mixer->rate));
            // A stem's own length, but no more than ten minutes past the last
            // note: a damaged last page can claim hours.
            const uint64_t most = static_cast<uint64_t>((mix.seconds + 600.0) * mixer->rate);
            uint64_t longest = 0u;
            for (const auto &in : mixer->inputs)
                longest = std::max(longest, std::min(in.pcm->frames, most));
            mixer->frames = static_cast<uint64_t>(std::max<int64_t>(int64_t(longest) - mixer->leadFrames, 0));
            // With no stem that opens, silence as long as the song, never no
            // file: the game has no song without its audio.
            if (mixer->inputs.empty())
            {
                std::cerr << "[custom] " << mix.path << ": no stem opens; silence" << std::endl;
                mixer->frames = static_cast<uint64_t>(std::max(mix.seconds, 1.0) * mixer->rate);
            }
            mixer->seek(0u);
            return mixer;
        }

        // The song's stems, the played part's on the second pair, every other
        // one backing on the first. A song of one stem, song.ogg or any, is
        // all the played part's, so a miss cuts it as it would a guitar.
        Mix mixFor(const std::string &path, const Song &song, const std::string &played)
        {
            Mix mix{path, {}, 0.0, song.length + 5.0, song.dir};
            for (const std::string &file : song.stems)
                mix.stems.emplace_back(songPath(song, file), song.stems.size() == 1u || partStem(file, played) ? 1u : 0u);
            // song.ini's delay, in ms, holds the notes back against the
            // audio, so the audio starts that far in.
            mix.lead = number(song.tags, "delay").value_or(0) / 1000.0;
            return mix;
        }

        // The preview played through once, as the game will play it
        // (song_files.cpp's preview), for every chunk it reads to be read.
        void readPreview(const Song &song)
        {
            const Mix mix = mixFor({}, song, {});
            auto pcm = mixOf(mix);
            const double length = song.length * 1000.0;
            const double from = std::clamp(double(number(song.tags, "preview_start_time").value_or(30000)), 0.0, std::max(length - 15000.0, 0.0));
            pcm->seek(static_cast<uint64_t>(from / 1000.0 * pcm->rate));
            std::vector<std::vector<float>> scratch(pcm->channels, std::vector<float>(4096u));
            std::vector<float *> out;
            for (auto &c : scratch)
                out.push_back(c.data());
            for (uint64_t left = static_cast<uint64_t>(17.0 * pcm->rate); left > 0u;)
            {
                const uint32_t n = pcm->fill(out.data(), static_cast<uint32_t>(std::min<uint64_t>(left, 4096u)));
                if (n == 0u)
                    break;
                left -= n;
            }
        }

        // The next run of a stem's chunks not read, up to a MB.
        bool nextRun(const Song &song, fs::path &file, StemPtr &stem, size_t &first, size_t &count)
        {
            Held *h = heldAt(song.dir);
            if (!h)
                return false;
            for (const auto &[path, s] : h->files)
            {
                const auto gap = std::find(s->chunks.begin(), s->chunks.end(), nullptr);
                if (gap == s->chunks.end())
                    continue;
                file = path;
                stem = s;
                first = static_cast<size_t>(gap - s->chunks.begin());
                count = 1u;
                while (count < 16u && first + count < s->chunks.size() && !s->chunks[first + count])
                    ++count;
                return true;
            }
            return false;
        }

        // One reader for all of it, so nothing waits behind a lesser read on
        // a slow disk: the art of the song on, its preview, the rest of its
        // stems for playing it while the list is still on it, then the art
        // of those about it.
        void reader()
        {
            std::unique_lock lock(s_mutex);
            for (;;)
            {
                const auto unread = [](const fs::path &f) { return !s_art.count(f); };
                const auto art = std::find_if(s_artWanted.begin(), s_artWanted.end(), unread);
                const std::shared_ptr<const Song> song = s_song;
                Held *h = song ? heldAt(song->dir) : nullptr;
                fs::path file;
                StemPtr stem;
                size_t first = 0u, count = 0u;
                if (art != s_artWanted.end() && art == s_artWanted.begin())
                    file = *art;
                else if (song && !(h && h->previewed))
                {
                    lock.unlock();
                    readPreview(*song);
                    lock.lock();
                    heldFor(song->dir).previewed = true;
                    s_listRead.notify_all();
                    continue;
                }
                // Its stems while on it, else the art about it, else its
                // stems all the same.
                else if (!(song && song->dir == s_onDir && nextRun(*song, file, stem, first, count)) &&
                         art != s_artWanted.end())
                    file = *art;
                else if (!stem && !(song && nextRun(*song, file, stem, first, count)))
                {
                    s_wake.wait(lock);
                    continue;
                }
                lock.unlock();
                if (stem)
                {
                    readChunks(file, *stem, first, count);
                    lock.lock();
                    continue;
                }
                Bytes bytes = readFile(file);
                lock.lock();
                if (std::find(s_artWanted.begin(), s_artWanted.end(), file) != s_artWanted.end())
                    s_art[file] = std::move(bytes);
                s_listRead.notify_all();
            }
        }

        // The list has a read still to come, under s_mutex.
        bool listWaits()
        {
            const Held *h = s_song ? heldAt(s_song->dir) : nullptr;
            return (s_song && !(h && h->previewed)) ||
                   std::any_of(s_artWanted.begin(), s_artWanted.end(), [](const fs::path &f) { return !s_art.count(f); });
        }

        void startReader()
        {
            static const bool started = []
            {
                std::thread(reader).detach();
                return true;
            }();
            (void)started;
        }
    }

    bool stemsReady(const Song &song)
    {
        startReader();
        const std::lock_guard lock(s_mutex);
        if (song.stems.empty())
            return true;
        if (!s_song || s_song->dir != song.dir)
        {
            s_song = std::make_shared<const Song>(song);
            s_wake.notify_one();
            s_listRead.notify_all();
        }
        if (Held *h = heldAt(song.dir); h && h->previewed)
        {
            heldFor(song.dir);
            return true;
        }
        return false;
    }

    void wantArt(const std::vector<std::shared_ptr<const Song>> &songs)
    {
        startReader();
        std::vector<fs::path> files;
        for (const auto &song : songs)
            if (!song->art.empty())
                files.push_back(songPath(*song, song->art));
        const std::lock_guard lock(s_mutex);
        s_onDir = songs.empty() ? fs::path() : songs.front()->dir;
        if (files == s_artWanted)
            return;
        s_artWanted = std::move(files);
        std::erase_if(s_art, [](const auto &art)
                      { return std::find(s_artWanted.begin(), s_artWanted.end(), art.first) == s_artWanted.end(); });
        s_wake.notify_one();
        s_listRead.notify_all();
    }

    void waitForList()
    {
        std::unique_lock lock(s_mutex);
        s_listRead.wait(lock, [] { return !listWaits(); });
    }

    std::shared_ptr<const std::vector<uint8_t>> heldArt(const Song &song)
    {
        if (song.art.empty())
            return nullptr;
        const std::lock_guard lock(s_mutex);
        const auto it = s_art.find(songPath(song, song.art));
        return it != s_art.end() ? it->second : nullptr;
    }

    std::optional<ark::Made> madeMix(const std::string &path, const Song &song, const std::string &played)
    {
        const Mix mix = mixFor(path, song, played);
        return vgs::made(path, [mix] { return mixOf(mix); });
    }
}
