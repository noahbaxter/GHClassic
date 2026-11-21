#include "host/song_log.h"

#include "dev/timing.h"
#include "host/log.h"
#include "ps2_runtime.h"
#include "render/texture_store.h"
#include "script.h"
#include "settings/settings.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <iostream>
#include <mutex>

namespace gh2::song_log
{
    namespace
    {
        using Clock = std::chrono::steady_clock;

        // A game frame held this long is one a player sees as a stall.
        constexpr float kHitchMs = 100.0f;
        // Lines for the first of a song's hitches; the rest only counted.
        constexpr int kHitchLines = 10;
        // The synth's: a lock this far off jumps the clock to the sound.
        constexpr float kSnapMs = 50.0f;
        // Sound asked for this late, against its buffer, ran dry.
        constexpr float kLateBuffers = 4.0f, kLateMs = 40.0f;
        // No new game frame for this long, song playing: frozen, said at once
        // in case it never comes back.
        constexpr auto kFrozen = std::chrono::seconds(3);

        // Milliseconds in quarter millisecond bins, the last all past 125.
        struct Histogram
        {
            std::array<uint32_t, 501> bins{};
            uint32_t count = 0;
            float max = 0.0f;

            void add(float ms)
            {
                ++bins[std::min<size_t>(static_cast<size_t>(std::max(ms, 0.0f) * 4.0f), bins.size() - 1u)];
                ++count;
                max = std::max(max, ms);
            }

            float at(double of) const
            {
                const uint32_t wanted = static_cast<uint32_t>(std::ceil(count * of));
                uint32_t seen = 0;
                for (size_t i = 0; i < bins.size(); ++i)
                    if ((seen += bins[i]) >= wanted && seen > 0u)
                        return i + 1u == bins.size() ? max : static_cast<float>(i) / 4.0f;
                return 0.0f;
            }

            // How many took longer than `ms`.
            uint32_t above(float ms) const
            {
                uint32_t n = 0;
                for (size_t i = std::min<size_t>(static_cast<size_t>(ms * 4.0f) + 1u, bins.size()); i < bins.size(); ++i)
                    n += bins[i];
                return n;
            }
        };

        std::mutex s_mutex;
        bool s_on = false;
        bool s_playing = false; // on the screen it is played on, no pause menu
        bool s_frozen = false;
        uint64_t (*s_gpuBytes)(void *) = nullptr;
        void *s_gpuOf = nullptr;
        textures::Reads s_readsAt;
        std::string s_what;
        const char *s_outcome = "left";
        Clock::time_point s_began, s_lastPresent, s_lastNew;
        // The loading screen's start, for how long it took to the song's;
        // none for a song started without one (practice's restarts).
        Clock::time_point s_loading;
        float s_songAt = -1.0f;
        Histogram s_frames, s_held;
        // Where each frame went: the GPU's passes, recording its draws, and
        // waiting on the game thread's frame or on the display.
        Histogram s_gpu, s_record, s_gameWait, s_displayWait;
        int s_hitches = 0;
        uint32_t s_locks = 0, s_snaps = 0;
        double s_errorSum = 0.0;
        float s_errorMax = 0.0f;
        uint32_t s_late = 0;
        float s_lateWorst = 0.0f;

        bool playedOn(const std::string &screen)
        {
            return screen == "game_screen" || screen == "practice_game_screen";
        }

        // Its pause menus, their settings and practice's speed picker.
        bool inSong(const std::string &screen)
        {
            return playedOn(screen) || screen.find("pause") != std::string::npos ||
                   screen == "practice_change_speed_screen";
        }

        // {ghc_song_start <song> <difficulty> <part> <mode> <players> <venue>}
        script::Node startCommand(const script::Call &call)
        {
            std::lock_guard lock(s_mutex);
            s_what = call.symbol(1) + " " + call.symbol(2) + " " + call.symbol(3) + ", " + call.symbol(4) + ", " +
                     std::to_string(static_cast<int>(call.number(5))) + "p, venue " + call.symbol(6);
            // The calibration it is played with, which can change mid-run.
            std::cerr << "[song] start " << s_what << ", video " << settings::get(settings::kVideoLagMs)
                      << " ms, audio " << settings::get(settings::kAudioLagMs) << " ms";
            if (s_loading != Clock::time_point{})
                std::cerr << ", loaded in "
                          << std::chrono::duration_cast<std::chrono::milliseconds>(s_began - s_loading).count() << " ms";
            std::cerr << std::endl;
            s_loading = {};
            return {};
        }

        // A play's counts from nothing. s_mutex held.
        void reset()
        {
            s_outcome = "left";
            s_began = Clock::now();
            s_lastPresent = s_lastNew = {};
            s_songAt = -1.0f;
            s_frames = s_held = s_gpu = s_record = s_gameWait = s_displayWait = {};
            s_hitches = 0;
            s_locks = s_snaps = 0;
            s_errorSum = 0.0;
            s_errorMax = 0.0f;
            s_late = 0;
            s_lateWorst = 0.0f;
            s_frozen = false;
            s_readsAt = textures::reads();
        }

        void begin()
        {
            std::lock_guard lock(s_mutex);
            // Counted from the song's clock moving, past the venue's load.
            timing::songSeconds(-1.0f);
            s_on = true;
            s_what = "?";
            reset();
        }

        // A play's line. s_mutex held.
        void report()
        {
            const int seconds = static_cast<int>(std::chrono::duration<float>(Clock::now() - s_began).count());
            const float gameFps = s_held.count > 0u ? 1000.0f / std::max(s_held.at(0.5), 0.25f) : 0.0f;
            const textures::Reads reads = textures::reads();
            constexpr uint64_t kMB = 1u << 20;
            const uint64_t gpu = s_gpuBytes ? s_gpuBytes(s_gpuOf) : 0u;
            // A frame half again the usual missed the display's refresh.
            const float usual = s_frames.at(0.5);
            char line[900];
            std::snprintf(line, sizeof line,
                          "[song] end %s, %s after %d:%02d: frames p50 %.1f p99 %.1f max %.0f ms, %u of %u missed; "
                          "game %.0f fps p99 %.1f ms, %d held over %.0f ms; per frame p50/p99 ms gpu %.1f/%.1f, "
                          "record %.1f/%.1f, wait for game %.1f/%.1f, wait for gpu+display %.1f/%.1f; "
                          "song clock to sound mean %+.1f max %.1f ms, %u jumps; "
                          "sound late %u times, worst %.0f ms; game thread read %llu textures in %llu ms; memory "
                          "%llu MB, gpu %llu MB",
                          s_what.c_str(), s_outcome, seconds / 60, seconds % 60, usual, s_frames.at(0.99),
                          s_frames.max, usual > 0.0f ? s_frames.above(usual * 1.5f) : 0u, s_frames.count, gameFps,
                          s_held.at(0.99), s_hitches, kHitchMs, s_gpu.at(0.5), s_gpu.at(0.99), s_record.at(0.5),
                          s_record.at(0.99), s_gameWait.at(0.5), s_gameWait.at(0.99), s_displayWait.at(0.5),
                          s_displayWait.at(0.99),
                          s_locks ? s_errorSum / s_locks : 0.0, s_errorMax, s_snaps, s_late, s_lateWorst,
                          static_cast<unsigned long long>(reads.count - s_readsAt.count),
                          static_cast<unsigned long long>((reads.ns - s_readsAt.ns) / 1000000u),
                          static_cast<unsigned long long>(log::memoryBytes() / kMB),
                          static_cast<unsigned long long>(gpu / kMB));
            std::cerr << line << std::endl;
        }

        void end(const char *outcome)
        {
            std::lock_guard lock(s_mutex);
            s_on = false;
            if (outcome)
                s_outcome = outcome;
            report();
        }
    }

    void install()
    {
        script::addCommand("ghc_song_start", startCommand);
    }

    void screen(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, const std::string &name)
    {
        bool on;
        {
            std::lock_guard lock(s_mutex);
            on = s_on;
            s_playing = playedOn(name);
            s_lastNew = {}; // a pause menu's wait is no freeze
            if (name == "loading_screen")
                s_loading = Clock::now();
        }
        if (on && !inSong(name))
            end(name == "practice_end_screen" ? "finished" : nullptr);
        else if (!on && playedOn(name))
        {
            begin();
            const R5900Context saved = *ctx;
            script::run(rdram, ctx, runtime,
                        "{ghc_song_start {game get_song} {game get_difficulty_sym 0} {gamecfg get_track_sym 0} "
                        "{gamecfg get mode} {game num_players} {game get_venue}}");
            *ctx = saved;
        }
    }

    void gameOver(bool won)
    {
        std::lock_guard lock(s_mutex);
        if (s_on)
            s_outcome = won ? "won" : "failed";
    }

    void quit()
    {
        // try_lock: exit() can come from a thread while another holds it.
        std::unique_lock lock(s_mutex, std::try_to_lock);
        if (!lock || !s_on)
            return;
        s_on = false;
        s_outcome = "quit";
        report();
    }

    void present(bool isNew, float gpuMs, float recordMs, float gameWaitMs, float displayWaitMs)
    {
        std::lock_guard lock(s_mutex);
        const Clock::time_point now = Clock::now();
        if (!s_on)
            return;
        // Restart, from the pause menu, goes back to the same screen: only
        // the song's clock going back tells it from a resume.
        const float song = timing::songSeconds();
        if (s_songAt > 1.0f && song < s_songAt - 1.0f)
        {
            s_outcome = "restarted";
            report();
            std::cerr << "[song] start " << s_what << " again" << std::endl;
            reset();
        }
        s_songAt = song;
        // Counted while it plays: not its intro, nor a pause menu's frames.
        if (song <= 0.0f || !s_playing)
        {
            s_lastPresent = s_lastNew = {};
            return;
        }
        if (s_lastPresent != Clock::time_point{})
            s_frames.add(std::chrono::duration<float, std::milli>(now - s_lastPresent).count());
        s_lastPresent = now;
        s_gpu.add(gpuMs);
        s_record.add(recordMs);
        s_gameWait.add(gameWaitMs);
        s_displayWait.add(displayWaitMs);
        if (!isNew)
        {
            if (s_playing && !s_frozen && s_lastNew != Clock::time_point{} && now - s_lastNew >= kFrozen)
            {
                s_frozen = true;
                char line[96];
                std::snprintf(line, sizeof line, "[freeze] no new game frame for 3 s, song at %.1f s, memory %llu MB",
                              song, static_cast<unsigned long long>(log::memoryBytes() >> 20));
                std::cerr << line << std::endl;
            }
            return;
        }
        s_frozen = false;
        if (s_lastNew != Clock::time_point{})
        {
            const float held = std::chrono::duration<float, std::milli>(now - s_lastNew).count();
            s_held.add(held);
            if (held >= kHitchMs && ++s_hitches <= kHitchLines)
            {
                char line[96];
                std::snprintf(line, sizeof line, "[hitch] %.0f ms held, song at %.1f s%s", held, timing::songSeconds(),
                              s_hitches == kHitchLines ? " (the song's last hitch line)" : "");
                std::cerr << line << std::endl;
            }
        }
        s_lastNew = now;
    }

    void gpuMemory(uint64_t (*bytes)(void *), void *of)
    {
        std::lock_guard lock(s_mutex);
        s_gpuBytes = bytes;
        s_gpuOf = of;
    }

    void clock(float errorMs, float pullMs)
    {
        std::lock_guard lock(s_mutex);
        if (!s_on)
            return;
        ++s_locks;
        s_errorSum += errorMs;
        s_errorMax = std::max(s_errorMax, std::fabs(errorMs));
        if (std::fabs(pullMs) > kSnapMs)
            ++s_snaps;
    }

    void audioPull(float gapMs, float periodMs)
    {
        if (gapMs < std::max(kLateMs, kLateBuffers * periodMs))
            return;
        std::lock_guard lock(s_mutex);
        if (!s_on)
            return;
        ++s_late;
        s_lateWorst = std::max(s_lateWorst, gapMs);
    }
}
