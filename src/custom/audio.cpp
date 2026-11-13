#include "custom/audio.h"

#include "content/audio_file.h"
#include "content/pcm_mix.h"
#include "custom/text.h"

#include <algorithm>
#include <cmath>
#include <condition_variable>
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

        Bytes readStem(const fs::path &file)
        {
            const std::string read = songFile(file);
            return std::make_shared<const std::vector<uint8_t>>(read.begin(), read.end());
        }

        // The stems of the last few songs read, a song's by its folder or
        // package, most recent first, and the files of the song stemsReady
        // last asked about, which the reader reads in turn.
        constexpr size_t kHeldSongs = 3u; // the one previewed, the one played, a spare
        struct Held
        {
            fs::path dir;
            std::map<fs::path, Bytes> files;
        };
        // Never destroyed: the reader outlives main's statics.
        std::mutex &s_heldMutex = *new std::mutex;
        std::condition_variable &s_wake = *new std::condition_variable;
        std::list<Held> &s_held = *new std::list<Held>;
        std::vector<fs::path> &s_wanted = *new std::vector<fs::path>;

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

        Bytes held(const fs::path &file)
        {
            for (const Held &h : s_held)
                if (const auto it = h.files.find(file); it != h.files.end())
                    return it->second;
            return nullptr;
        }

        void reader()
        {
            std::unique_lock lock(s_heldMutex);
            for (;;)
            {
                const auto next = std::find_if(s_wanted.begin(), s_wanted.end(), [](const fs::path &f) { return !held(f); });
                if (next == s_wanted.end())
                {
                    s_wake.wait(lock);
                    continue;
                }
                const fs::path file = *next;
                lock.unlock();
                Bytes bytes = readStem(file);
                lock.lock();
                // Kept only while still wanted: a song scrolled past is not
                // to push out the one now previewed or played.
                if (std::find(s_wanted.begin(), s_wanted.end(), file) != s_wanted.end())
                    heldFor(file.parent_path()).files[file] = std::move(bytes);
            }
        }

        std::unique_ptr<vgs::Pcm> openStem(const fs::path &file)
        {
            Bytes read;
            {
                const std::lock_guard lock(s_heldMutex);
                read = held(file);
            }
            if (!read)
            {
                read = readStem(file);
                const std::lock_guard lock(s_heldMutex);
                heldFor(file.parent_path()).files[file] = read;
            }
            if (read->empty())
            {
                std::cerr << "[custom] cannot read " << utf8(file) << std::endl;
                return nullptr;
            }
            return audio_file::open(std::move(read), utf8(file));
        }
    }

    bool stemsReady(const Song &song)
    {
        static const bool started = []
        {
            std::thread(reader).detach();
            return true;
        }();
        (void)started;
        std::vector<fs::path> files;
        if (!song.art.empty())
            files.push_back(songPath(song, song.art));
        for (const std::string &file : song.stems)
            files.push_back(songPath(song, file));
        const std::lock_guard lock(s_heldMutex);
        if (std::all_of(files.begin(), files.end(), [](const fs::path &f) { return held(f) != nullptr; }))
        {
            if (!files.empty())
                heldFor(song.dir);
            return true;
        }
        if (files != s_wanted)
        {
            s_wanted = std::move(files);
            s_wake.notify_one();
        }
        return false;
    }

    std::shared_ptr<const std::vector<uint8_t>> heldArt(const Song &song)
    {
        if (song.art.empty())
            return nullptr;
        const std::lock_guard lock(s_heldMutex);
        return held(songPath(song, song.art));
    }

    std::optional<ark::Made> madeMix(const std::string &path, const Song &song, const std::string &played)
    {
        // The played part's stems on the second pair, every other one
        // backing on the first. A song of one stem, song.ogg or any, is
        // all the played part's, so a miss cuts it as it would a guitar.
        std::vector<std::pair<fs::path, uint32_t>> stems;
        for (const std::string &file : song.stems)
            stems.emplace_back(songPath(song, file), song.stems.size() == 1u || partStem(file, played) ? 1u : 0u);
        // song.ini's delay, in ms, holds the notes back against the audio,
        // so the audio starts that far in.
        const double lead = number(song.tags, "delay").value_or(0) / 1000.0;
        // A few seconds past the last note for the silence, as a song's
        // audio runs on after it.
        const double seconds = song.length + 5.0;
        return vgs::made(path, [path, stems, lead, seconds]() -> std::unique_ptr<vgs::Pcm>
                   {
                       auto mixer = std::make_unique<vgs::Mixer>();
                       for (const auto &[file, pair] : stems)
                           if (auto pcm = openStem(file))
                               mixer->inputs.push_back({std::move(pcm), pair});
                       // The mix at the first stem's rate, any other read to it.
                       mixer->rate = mixer->inputs.empty() ? 44100u : mixer->inputs.front().pcm->rate;
                       for (auto &in : mixer->inputs)
                           if (in.pcm->rate != mixer->rate)
                               in.pcm = std::make_unique<vgs::Resampled>(std::move(in.pcm), mixer->rate);
                       mixer->channels = 4u;
                       mixer->leadFrames = static_cast<int64_t>(std::llround(lead * mixer->rate));
                       // A stem's own length, but no more than ten minutes
                       // past the last note: a damaged last page can claim
                       // hours.
                       const uint64_t most = static_cast<uint64_t>((seconds + 600.0) * mixer->rate);
                       uint64_t longest = 0u;
                       for (const auto &in : mixer->inputs)
                           longest = std::max(longest, std::min(in.pcm->frames, most));
                       mixer->frames = static_cast<uint64_t>(std::max<int64_t>(int64_t(longest) - mixer->leadFrames, 0));
                       // With no stem that opens, silence as long as the
                       // song, never no file: the game has no song without
                       // its audio.
                       if (mixer->inputs.empty())
                       {
                           std::cerr << "[custom] " << path << ": no stem opens; silence" << std::endl;
                           mixer->frames = static_cast<uint64_t>(std::max(seconds, 1.0) * mixer->rate);
                       }
                       mixer->seek(0u);
                       return mixer;
                   });
    }
}
