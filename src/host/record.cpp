#include "host/record.h"

#include "host/audio.h"

#include <condition_variable>
#include <cstdio>
#include <deque>
#include <filesystem>
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

        std::mutex s_mutex;
        std::condition_variable s_changed;
        std::string s_path;
        FILE *s_video = nullptr; // ffmpeg's stdin
        FILE *s_sound = nullptr; // raw f32le beside the .mp4
        std::chrono::steady_clock::time_point s_start{};
        uint64_t s_written = 0, s_repeated = 0, s_dropped = 0;
        bool s_started = false, s_finished = false;
        // Held, and held since the last frame: the next one carries on.
        bool s_held = false, s_resumed = false;

        // Frames on their way to ffmpeg, so the game never waits on it: its
        // encoder takes a moment to start, and runs beside the game after.
        // A second's worth at most; past that the game waits.
        constexpr size_t kQueued = 60u;
        std::deque<Picture> s_queue;
        std::thread s_writer;

        std::string videoPath() { return s_path + ".video.mp4"; }
        std::string soundPath() { return s_path + ".sound.f32"; }

        void write()
        {
            std::unique_lock<std::mutex> lock(s_mutex);
            while (true)
            {
                s_changed.wait(lock, [] { return !s_queue.empty() || s_finished; });
                if (s_queue.empty())
                    return;
                const Picture picture = std::move(s_queue.front());
                s_queue.pop_front();
                s_changed.notify_all();
                lock.unlock();
                std::fwrite(picture->data(), 1, picture->size(), s_video);
                lock.lock();
            }
        }
    }

    void setPath(const std::string &path) { s_path = path; }

    bool on() { return !s_path.empty(); }

    void hold(bool held)
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        s_resumed = s_resumed || (s_held && !held);
        s_held = held;
    }

    void frame(const uint8_t *rgba, uint32_t width, uint32_t height, std::chrono::steady_clock::time_point at,
               std::chrono::nanoseconds period)
    {
        std::unique_lock<std::mutex> lock(s_mutex);
        if (s_path.empty() || s_finished || s_held)
            return;
        // Back from a hold: this frame is the next in the video.
        if (s_resumed && s_started)
            s_start = at - period * static_cast<int64_t>(s_written);
        s_resumed = false;
        if (!s_started)
        {
            // VideoToolbox: the Mac's own encoder, far faster than the game.
            // At the game's own rate, so the picture keeps time with the sound.
            char rate[32];
            std::snprintf(rate, sizeof(rate), "%.6f", 1e9 / static_cast<double>(period.count()));
            const std::string command = "ffmpeg -hide_banner -loglevel error -y -f rawvideo -pix_fmt rgba -s " +
                                        std::to_string(width) + "x" + std::to_string(height) + " -r " + rate +
                                        " -i - -c:v h264_videotoolbox -b:v 30M -pix_fmt yuv420p \"" + videoPath() +
                                        "\"";
            s_video = popen(command.c_str(), "w");
            s_sound = std::fopen(soundPath().c_str(), "wb");
            if (!s_video || !s_sound)
            {
                std::cerr << "[record] cannot start ffmpeg or " << soundPath() << std::endl;
                s_path.clear();
                return;
            }
            s_start = at;
            s_started = true;
            s_writer = std::thread(write);
        }
        // Its place in the video, by the game's clock.
        const auto place = static_cast<uint64_t>((at - s_start + period / 2) / period);
        if (place < s_written)
        {
            ++s_dropped;
            return;
        }
        const auto picture =
            std::make_shared<const std::vector<uint8_t>>(rgba, rgba + size_t(width) * height * 4u);
        while (s_written <= place)
        {
            s_changed.wait(lock, [] { return s_queue.size() < kQueued; });
            s_queue.push_back(picture);
            s_changed.notify_all();
            s_repeated += s_written < place;
            ++s_written;
        }
    }

    void audio(const float *stereo, size_t frames)
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        if (s_started && !s_finished && !s_held && !s_resumed)
            std::fwrite(stereo, sizeof(float), frames * 2u, s_sound);
    }

    void finish()
    {
        {
            std::lock_guard<std::mutex> lock(s_mutex);
            if (!s_started || s_finished)
                return;
            s_finished = true;
        }
        s_changed.notify_all();
        s_writer.join();
        pclose(s_video);
        std::fclose(s_sound);
        const std::string command = "ffmpeg -hide_banner -loglevel error -y -i \"" + videoPath() +
                                    "\" -f f32le -ar " + std::to_string(kAudioRate) + " -ac 2 -i \"" + soundPath() +
                                    "\" -c:v copy -c:a aac -b:a 192k -shortest \"" + s_path + "\"";
        const int status = std::system(command.c_str());
        std::filesystem::remove(videoPath());
        std::filesystem::remove(soundPath());
        std::cerr << "[record] " << s_path << (status == 0 ? "" : " failed") << ": " << s_written << " frames, "
                  << s_repeated << " repeated and " << s_dropped
                  << " dropped (any repeated: the game fell behind its clock, so a lower --speed)" << std::endl;
    }
}
