#include "host/record.h"

#include "host/audio.h"
#include "runtime/host_clock.h"

#include <atomic>
#include <condition_variable>
#include <csignal>
#include <cstdio>
#include <deque>
#include <iostream>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace gh2::record
{
    namespace
    {
        using Picture = std::shared_ptr<const std::vector<uint8_t>>;
        // A picture for a clip's ffmpeg, or with none, the clip's end: its pipe closed.
        struct Item
        {
            Picture picture;
            FILE *video;
        };

        std::mutex s_mutex;
        std::condition_variable s_changed;
        std::string s_dir;
        std::atomic<bool> s_wanted{true};
        std::string s_next; // the next clip's name, if given
        int s_clips = 0;
        bool s_finished = false;

        // The clip on: ffmpeg's stdin, its sound, its first frame on the
        // game's clock and its counts.
        std::string s_clip;
        FILE *s_video = nullptr;
        FILE *s_sound = nullptr;
        std::chrono::steady_clock::time_point s_start{};
        std::chrono::nanoseconds s_period{};
        uint64_t s_written = 0, s_repeated = 0, s_dropped = 0, s_samples = 0;

        // Frames on their way to ffmpeg, so the game never waits on it: its
        // encoder takes a moment to start, and runs beside the game after.
        // A second's worth at most; past that the game waits.
        constexpr size_t kQueued = 60u;
        std::deque<Item> s_queue;
        std::thread s_writer;

        void write()
        {
            std::unique_lock<std::mutex> lock(s_mutex);
            while (true)
            {
                s_changed.wait(lock, [] { return !s_queue.empty() || s_finished; });
                if (s_queue.empty())
                    return;
                const Item item = std::move(s_queue.front());
                s_queue.pop_front();
                s_changed.notify_all();
                lock.unlock();
                if (item.picture)
                    std::fwrite(item.picture->data(), 1, item.picture->size(), item.video);
                else
                    pclose(item.video);
                lock.lock();
            }
        }

        // Under s_mutex: the clip on, ended. Its pipe closes behind the
        // frames still queued for it.
        void end()
        {
            if (!s_video)
                return;
            s_queue.push_back({nullptr, s_video});
            s_changed.notify_all();
            std::fclose(s_sound);
            s_video = s_sound = nullptr;
            std::cerr << "[record] " << s_clip << ": " << s_written << " frames ("
                      << std::chrono::duration<double>(s_period * s_written).count() << " s, sound "
                      << static_cast<double>(s_samples) / kAudioRate << " s), " << s_repeated << " repeated and "
                      << s_dropped << " dropped (any repeated: the game fell behind its clock, so a lower --speed)"
                      << std::endl;
        }

        // Under s_mutex: a clip from this frame.
        bool begin(uint32_t width, uint32_t height, std::chrono::steady_clock::time_point at,
                   std::chrono::nanoseconds period)
        {
            char number[16];
            std::snprintf(number, sizeof(number), "%02d", ++s_clips);
            s_clip = s_next.empty() ? number : s_next;
            s_next.clear();
            const std::string path = s_dir + "/" + s_clip;
            // VideoToolbox: the Mac's own encoder, far faster than the game.
            // At the game's own rate, so the picture keeps time with the sound.
            char rate[32];
            std::snprintf(rate, sizeof(rate), "%.6f", 1e9 / static_cast<double>(period.count()));
            const std::string command = "ffmpeg -hide_banner -loglevel error -y -f rawvideo -pix_fmt rgba -s " +
                                        std::to_string(width) + "x" + std::to_string(height) + " -r " + rate +
                                        " -i - -c:v h264_videotoolbox -b:v 30M -pix_fmt yuv420p \"" + path +
                                        ".ts\"";
            s_video = popen(command.c_str(), "w");
            s_sound = std::fopen((path + ".f32").c_str(), "wb");
            if (!s_video || !s_sound)
            {
                std::cerr << "[record] cannot start ffmpeg or " << path << ".f32" << std::endl;
                if (s_video)
                    pclose(s_video);
                if (s_sound)
                    std::fclose(s_sound);
                s_video = s_sound = nullptr;
                s_dir.clear();
                return false;
            }
            s_start = at;
            s_period = period;
            s_written = s_repeated = s_dropped = s_samples = 0u;
            if (!s_writer.joinable())
                s_writer = std::thread(write);
            return true;
        }
    }

    void setPath(const std::string &dir)
    {
        s_dir = dir;
#ifdef SIGPIPE
        // An ffmpeg that dies fails its writes rather than ending the game.
        std::signal(SIGPIPE, SIG_IGN);
#endif
    }

    void start(const std::string &name)
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        end();
        s_next = name;
        s_wanted = true;
    }

    void stop()
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        end();
        s_wanted = false;
    }

    bool wanted() { return s_wanted.load(std::memory_order_relaxed) && !s_dir.empty(); }

    void frame(const uint8_t *rgba, uint32_t width, uint32_t height, std::chrono::steady_clock::time_point at,
               std::chrono::nanoseconds period)
    {
        std::unique_lock<std::mutex> lock(s_mutex);
        if (s_dir.empty() || s_finished || !s_wanted)
            return;
        if (!s_video && !begin(width, height, at, period))
            return;
        // Its place in the clip, by the game's clock.
        const auto place = static_cast<uint64_t>((at - s_start + period / 2) / period);
        if (place < s_written)
        {
            ++s_dropped;
            return;
        }
        const auto picture =
            std::make_shared<const std::vector<uint8_t>>(rgba, rgba + size_t(width) * height * 4u);
        FILE *const video = s_video;
        while (s_written <= place)
        {
            s_changed.wait(lock, [] { return s_queue.size() < kQueued; });
            // Ended while it waited: its pipe is closing.
            if (s_video != video)
                return;
            s_queue.push_back({picture, video});
            s_changed.notify_all();
            s_repeated += s_written < place;
            ++s_written;
        }
    }

    void audio(const float *stereo, size_t frames)
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        if (!s_sound)
            return;
        // Its place in the clip, by the game's clock, as the frames have
        // theirs. Time the mixer missed (its thread held up, as at boot) is
        // silence, and sound past its place is dropped, so the sound keeps
        // time with the picture.
        const double at = std::chrono::duration<double>(ps2x::host_clock::now() - s_start).count();
        const int64_t late = static_cast<int64_t>(at * kAudioRate) - static_cast<int64_t>(frames + s_samples);
        constexpr int64_t kSlack = kAudioRate / 10u;
        if (late < -kSlack)
            return;
        if (late > kSlack)
        {
            const std::vector<float> silence(static_cast<size_t>(late) * 2u, 0.0f);
            std::fwrite(silence.data(), sizeof(float), silence.size(), s_sound);
            s_samples += static_cast<uint64_t>(late);
        }
        std::fwrite(stereo, sizeof(float), frames * 2u, s_sound);
        s_samples += frames;
    }

    void finish()
    {
        {
            std::lock_guard<std::mutex> lock(s_mutex);
            if (s_finished)
                return;
            end();
            s_finished = true;
        }
        s_changed.notify_all();
        if (s_writer.joinable())
            s_writer.join();
    }
}
