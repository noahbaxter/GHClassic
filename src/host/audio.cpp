#include "host/audio.h"
#include "runtime/host_clock.h"

#include <SDL3/SDL.h>

#include <atomic>
#include <chrono>
#include <iostream>
#include <thread>
#include <vector>

namespace gh2
{
    namespace
    {
        std::atomic<AudioSource> s_source{nullptr};
        std::atomic<AudioSource> s_movieSource{nullptr};
        SDL_AudioStream *s_stream = nullptr;
        std::thread s_timer;
        std::atomic<bool> s_timerRunning{false};

        void render(std::vector<float> &buffer, size_t frames)
        {
            buffer.assign(frames * 2u, 0.0f);
            if (AudioSource source = s_source.load(std::memory_order_acquire))
                source(buffer.data(), frames);
            if (AudioSource movie = s_movieSource.load(std::memory_order_acquire))
                movie(buffer.data(), frames);
        }

        // SDL asks for bytes as the device drains, and gets exactly that many.
        void pull(void *, SDL_AudioStream *stream, int additional, int)
        {
            static std::vector<float> buffer;
            const size_t frames = static_cast<size_t>(additional) / (2u * sizeof(float));
            if (frames == 0u)
                return;
            render(buffer, frames);
            SDL_PutAudioStreamData(stream, buffer.data(), static_cast<int>(frames * 2u * sizeof(float)));
        }

        // Muted: 10 ms at a time against the host clock, as a device would.
        void runTimer()
        {
            constexpr size_t kFrames = kAudioRate / 100u;
            std::vector<float> buffer;
            auto next = ps2x::host_clock::now();
            while (s_timerRunning.load(std::memory_order_relaxed))
            {
                next += std::chrono::milliseconds(10);
                std::this_thread::sleep_until(ps2x::host_clock::real(next));
                render(buffer, kFrames);
            }
        }
    }

    void openAudio(bool mute)
    {
        if (mute)
        {
            s_timerRunning = true;
            s_timer = std::thread(runTimer);
            return;
        }
        // About 10 ms a device period: the note chart is timed against it.
        SDL_SetHint(SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES, "512");
        if (!SDL_InitSubSystem(SDL_INIT_AUDIO))
        {
            std::cerr << "[audio] init: " << SDL_GetError() << std::endl;
            return;
        }
        const SDL_AudioSpec spec{SDL_AUDIO_F32, 2, static_cast<int>(kAudioRate)};
        s_stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, pull, nullptr);
        if (!s_stream)
        {
            std::cerr << "[audio] open: " << SDL_GetError() << std::endl;
            SDL_QuitSubSystem(SDL_INIT_AUDIO);
            return;
        }
        SDL_ResumeAudioStreamDevice(s_stream);
    }

    void closeAudio()
    {
        if (s_timer.joinable())
        {
            s_timerRunning = false;
            s_timer.join();
        }
        if (s_stream)
        {
            SDL_DestroyAudioStream(s_stream);
            s_stream = nullptr;
            SDL_QuitSubSystem(SDL_INIT_AUDIO);
        }
    }

    void setAudioSource(AudioSource source)
    {
        s_source.store(source, std::memory_order_release);
    }

    void setMovieAudioSource(AudioSource source)
    {
        s_movieSource.store(source, std::memory_order_release);
    }
}
