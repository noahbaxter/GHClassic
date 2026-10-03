#include "dev/scenario.h"

#include "content/games.h"
#include "dev/transplant.h"
#include "frame_step.h"
#include "dev/draw_dump.h"
#include "ps2_runtime_macros.h"
#include "guest.h"
#include "hook.h"
#include "host/pad.h"
#include "host/vulkan_frontend.h"
#include "movie/movie.h"
#include "script.h"

#include <SDL3/SDL.h>

#include "ps2_runtime.h"
#include "runtime/ee_scheduler.h"
#include "runtime/host_clock.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <thread>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>

namespace gh2::scenario
{
    namespace
    {
        using Clock = std::chrono::steady_clock;

        constexpr uint32_t kString = 0x12u;
        constexpr int kTimeout = 30; // seconds

        std::string s_text;
        uint32_t s_steps = 0u; // the parsed file, a DataArray
        int s_next = 0;
        Clock::time_point s_start;
        Clock::time_point s_until;    // a wait's end
        bool s_pressed = false;       // a press's button still held
        Clock::time_point s_release;  // and when it lets go
        std::string s_screen;         // a wait_screen's screen
        uint32_t s_condition = 0u;    // a wait_until's step array
        bool s_shooting = false;      // a shot's frame not yet written
        Clock::time_point s_deadline; // and when either fails
        int s_timeout = kTimeout;     // that deadline's seconds, to report
        std::atomic<bool> s_done{false};
        bool s_frozen = false;  // a freeze holds TaskMgr's clocks
        float s_uiSeconds = 0.0f, s_seconds = 0.0f, s_beat = 0.0f; // at these
        // A clock steps the UI clock 1/64 s a poll: a step floats hold exactly,
        // so the PS2's rounding and the host's give the same times.
        bool s_clock = false;
        float s_clockBase = 0.0f;
        int s_clockPolls = 0;
        const Addresses *s_addresses = nullptr;
        std::string s_transplantName; // a transplant's shot and dump
        int s_transplantPolls = 0;    // polls since it

        // Steps wait in game time, which --speed runs faster than real. The
        // timeouts stay real: a game behind its clock is not hung.
        Clock::time_point now() { return ps2x::host_clock::now(); }

        double seconds()
        {
            return std::chrono::duration<double>(now() - s_start).count();
        }

        void quit()
        {
            SDL_Event event{};
            event.type = SDL_EVENT_QUIT;
            SDL_PushEvent(&event);
            s_done = true;
        }

        // A line "#include <file>" is replaced by that file, beside this one.
        // The game's parser would look for it on the disc.
        bool read(const std::filesystem::path &path, std::string &out)
        {
            std::ifstream in(path);
            if (!in)
            {
                std::cerr << "[scenario] cannot read " << path.string() << std::endl;
                return false;
            }
            std::string line;
            while (std::getline(in, line))
            {
                if (line.rfind("#include ", 0) == 0)
                {
                    if (!read(path.parent_path() / line.substr(9), out))
                        return false;
                }
                else
                    out += line + "\n";
            }
            return true;
        }

        std::string text(uint8_t *rdram, const script::Node &node)
        {
            switch (node.type)
            {
            case script::kInt:
                return std::to_string(static_cast<int32_t>(node.value));
            case script::kFloat:
            {
                float f;
                std::memcpy(&f, &node.value, sizeof(f));
                return std::to_string(f);
            }
            case script::kSymbol:
                return node.value ? reinterpret_cast<const char *>(getMemPtr(rdram, node.value)) : "";
            case kString:
            {
                // A string node holds a DataArray whose first word is the text.
                const uint32_t chars = node.value ? gh2::load<uint32_t>(rdram, node.value) : 0u;
                return chars ? reinterpret_cast<const char *>(getMemPtr(rdram, chars)) : "";
            }
            case script::kArray:
                return "<array>";
            default:
                return "<type " + std::to_string(node.type) + ">";
            }
        }

        // The current screen's name, or "" during a transition or with none.
        std::string currentScreen(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            static uint32_t probe = 0u;
            if (probe == 0u)
                probe = script::parse(rdram, ctx, runtime,
                                      "{if_else {|| {ui in_transition} {! {ui current_screen}}} \"\" "
                                      "{{ui current_screen} name}}");
            const script::Call call{rdram, ctx, runtime, probe};
            return text(rdram, call.arg(0));
        }

        // Runs steps until one waits.
        void step(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const script::Call steps{rdram, ctx, runtime, s_steps};
            if (s_shooting)
            {
                if (shotPending() || drawDumpPending())
                    return;
                s_shooting = false;
            }
            if (!s_screen.empty())
            {
                const std::string current = currentScreen(rdram, ctx, runtime);
                if (current != s_screen)
                {
                    if (Clock::now() > s_deadline)
                    {
                        std::cerr << "[scenario] FAIL: no " << s_screen << " after " << s_timeout << " s, on '"
                                  << current << "'" << std::endl;
                        quit();
                    }
                    return;
                }
                s_screen.clear();
            }
            if (s_condition != 0u)
            {
                const script::Call condition{rdram, ctx, runtime, s_condition};
                const script::Node value = condition.arg(1);
                if (value.type == script::kInt && value.value == 0u)
                {
                    if (Clock::now() > s_deadline)
                    {
                        std::cerr << "[scenario] FAIL: wait_until still false after " << s_timeout << " s"
                                  << std::endl;
                        quit();
                    }
                    return;
                }
                s_condition = 0u;
            }
            if (s_pressed && now() >= s_release)
            {
                setScriptedPad(0u);
                s_pressed = false;
            }
            if (now() < s_until)
                return;

            while (s_next < steps.size())
            {
                const int i = s_next++;
                const script::Node node = steps.arg(i); // evaluates a command
                if (node.type != script::kArray)
                    continue;
                const script::Call array{rdram, ctx, runtime, node.value};
                const std::string verb = array.symbol(0);
                std::cerr << "[scenario] " << seconds() << " s: (" << verb << ")" << std::endl;
                if (verb == "wait")
                {
                    s_until = now() + std::chrono::milliseconds(static_cast<int>(array.number(1) * 1000.0f));
                    return;
                }
                if (verb == "press")
                {
                    // Held long enough for the pad to be read, then let go
                    // before the next step.
                    setScriptedPad(padButton(array.symbol(1)));
                    s_pressed = true;
                    s_release = now() + std::chrono::milliseconds(150);
                    s_until = now() + std::chrono::milliseconds(400);
                    return;
                }
                if (verb == "wait_screen" || verb == "wait_until")
                {
                    if (verb == "wait_screen")
                        s_screen = array.symbol(1);
                    else
                        s_condition = node.value;
                    s_timeout = array.size() > 2 ? static_cast<int>(array.number(2)) : kTimeout;
                    s_deadline = Clock::now() + std::chrono::seconds(s_timeout);
                    return;
                }
                if (verb == "print")
                {
                    std::string line;
                    for (int j = 1; j < array.size(); ++j)
                        line += (j > 1 ? " " : "") + text(rdram, array.arg(j));
                    std::cerr << "[scenario] " << line << std::endl;
                }
                else if (verb == "expect")
                {
                    const std::string got = text(rdram, array.arg(1));
                    const std::string wanted = text(rdram, array.arg(2));
                    // With a tolerance, numbers within it; else the same text.
                    const bool ok = array.size() > 3
                                        ? std::fabs(std::strtod(got.c_str(), nullptr) -
                                                    std::strtod(wanted.c_str(), nullptr)) <= array.number(3)
                                        : got == wanted;
                    if (!ok)
                    {
                        std::cerr << "[scenario] FAIL: expected " << wanted << ", got " << got << std::endl;
                        quit();
                        return;
                    }
                    std::cerr << "[scenario] ok " << got << std::endl;
                }
                else if (verb == "hold")
                    // Pad buttons held until the next (hold), libpad bits: (hold 0) lets go.
                    setScriptedPad(static_cast<uint16_t>(array.number(1)));
                else if (verb == "stall")
                    // A hitch: the game thread stops while audio plays on.
                    std::this_thread::sleep_for(std::chrono::milliseconds(static_cast<int>(array.number(1) * 1000.0f)));
                else if (verb == "freeze")
                {
                    s_uiSeconds = array.number(1);
                    s_seconds = array.number(2);
                    s_beat = array.number(3);
                    s_frozen = true;
                    freezeFrameSteps();
                    // GamePanel leaves these alone while paused: current and last, at TaskMgr +0x28.
                    const uint32_t timers = gh2::load<uint32_t>(rdram, s_addresses->theTaskMgr + 0x28u);
                    for (uint32_t at : {0xcu, 0x10u})
                    {
                        gh2::store<float>(rdram, timers + at, s_seconds);
                        gh2::store<float>(rdram, timers + 0x14u + at, s_beat);
                    }
                }
                else if (verb == "clock")
                {
                    s_clockBase = array.number(1);
                    s_clockPolls = 0;
                    s_clock = true;
                }
                else if (verb == "shot")
                {
                    // Nothing more runs until that frame is written.
                    requestShot(array.symbol(1));
                    s_shooting = true;
                    return;
                }
                else if (verb == "poke")
                {
                    // A word of the object the first argument evaluates to: an int or a float's bits.
                    const uint32_t object = array.arg(1).value;
                    if (object != 0u)
                        gh2::store<uint32_t>(rdram, object + static_cast<uint32_t>(static_cast<int32_t>(array.number(2))),
                                             array.arg(3).value);
                }
                else if (verb == "transplant")
                {
                    // Copied out first: both are in the memory about to go.
                    const std::string path = text(rdram, array.arg(1));
                    s_transplantName = array.symbol(2);
                    if (!transplant::load(rdram, path))
                    {
                        std::cerr << "[scenario] FAIL: transplant" << std::endl;
                        quit();
                    }
                    // The first frame drawn from it: later ones lack what a
                    // poll would have set up again.
                    requestDrawDump(s_transplantName, building().serial + 1u);
                    return;
                }
                else if (verb == "peek")
                {
                    const uint32_t object = array.arg(1).value;
                    const uint32_t word =
                        object != 0u ? gh2::load<uint32_t>(
                                           rdram, object + static_cast<uint32_t>(static_cast<int32_t>(array.number(2))))
                                     : 0u;
                    float f;
                    std::memcpy(&f, &word, sizeof(f));
                    std::cerr << "[scenario] peek " << word << " " << f << std::endl;
                }
                else if (verb == "dump")
                {
                    requestDrawDump(array.symbol(1));
                    s_shooting = true;
                    return;
                }
                else if (verb == "needs")
                {
                    std::string missing;
                    for (int j = 1; j < array.size(); ++j)
                        if (!games::mounted(array.symbol(j)))
                            missing += (missing.empty() ? "" : " ") + array.symbol(j);
                    if (!missing.empty())
                    {
                        std::cerr << "[scenario] SKIP: needs " << missing << std::endl;
                        quit();
                        return;
                    }
                }
                else if (verb == "quit")
                {
                    quit();
                    return;
                }
                else
                    std::cerr << "[scenario] unknown step (" << verb << ")" << std::endl;
            }
            std::cerr << "[scenario] done at " << seconds() << " s" << std::endl;
            s_done = true;
        }

        std::atomic<int64_t> s_lastPoll{0}; // steady_clock ticks
        std::atomic<PS2Runtime *> s_runtime{nullptr};
        std::atomic<uint8_t *> s_rdram{nullptr};

        // The UI stops polling when the game thread blocks: say on what,
        // once, from the kernel's own view of its threads, and end the run.
        void watchdog()
        {
            static const char *const kStatus[] = {"running", "ready", "waiting", "waiting-suspended", "suspended",
                                                  "dormant"};
            static const char *const kWait[] = {"", "sleep", "sema", "eventflag", "vsync", "external", "mpeg"};
            for (;;)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                PS2Runtime *runtime = s_runtime.load();
                if (s_done || !runtime)
                    continue;
                // A movie holds the game thread for its whole length.
                if (moviePlaying())
                {
                    s_lastPoll = Clock::now().time_since_epoch().count();
                    continue;
                }
                const auto idle = Clock::now() - Clock::time_point(Clock::duration(s_lastPoll.load()));
                // Boot and screen loads pause it a few seconds.
                if (idle < std::chrono::seconds(8))
                    continue;
                const EeKernelSnapshot snapshot = runtime->eeScheduler().snapshot();
                std::cerr << "[scenario] STALL: no UI poll for 8 s at " << seconds() << " s" << std::endl;
                for (const EeThreadSnapshot &t : snapshot.threads)
                    std::cerr << std::hex << "[scenario]   thread " << std::dec << t.id << " "
                              << kStatus[static_cast<int>(t.status)] << " " << kWait[static_cast<int>(t.waitReason)]
                              << " " << t.waitId << std::hex << " pc 0x" << t.pc << " ra 0x" << t.ra << std::dec
                              << std::endl;
                // A running thread's stack, nearest first: the words that
                // point into code, as a rough backtrace, and each script
                // array there by its leading symbols.
                for (const EeThreadSnapshot &t : snapshot.threads)
                {
                    uint8_t *rdram = s_rdram.load();
                    if (t.status != EeThreadStatus::Running || !rdram || t.sp == 0u)
                        continue;
                    std::cerr << "[scenario]   stack" << std::hex;
                    int shown = 0;
                    for (uint32_t at = t.sp; at < t.sp + 0x1000u && shown < 24; at += 4u)
                        if (const uint32_t word = gh2::load<uint32_t>(rdram, at);
                            word >= 0x100000u && word < 0x3d0000u && (word & 3u) == 0u)
                            std::cerr << " 0x" << word, ++shown;
                    std::cerr << std::dec << std::endl;
                    shown = 0;
                    for (uint32_t at = t.sp; at < t.sp + 0x1000u && shown < 12; at += 4u)
                    {
                        const uint32_t array = gh2::load<uint32_t>(rdram, at);
                        if (array < 0x400000u || array >= 0x2000000u || (array & 3u))
                            continue;
                        const uint32_t nodes = gh2::load<uint32_t>(rdram, array);
                        const int count = gh2::load<int16_t>(rdram, array + 8u);
                        if (nodes < 0x400000u || nodes >= 0x2000000u || (nodes & 3u) || count < 1 || count > 64 ||
                            gh2::load<uint32_t>(rdram, nodes + 4u) != script::kSymbol)
                            continue;
                        std::cerr << "[scenario]   script {";
                        for (int i = 0; i < count && i < 4; ++i)
                        {
                            const uint32_t value = gh2::load<uint32_t>(rdram, nodes + 8u * i);
                            if (gh2::load<uint32_t>(rdram, nodes + 8u * i + 4u) != script::kSymbol || value < 0x100000u ||
                                value >= 0x2000000u)
                                break;
                            const char *text = reinterpret_cast<const char *>(getMemPtr(rdram, value));
                            std::cerr << " " << std::string(text, strnlen(text, 40));
                        }
                        std::cerr << " ...}" << std::endl;
                        ++shown;
                    }
                }
                for (const EeSemaphoreSnapshot &s : snapshot.semaphores)
                    std::cerr << "[scenario]   sema " << s.id << " count " << s.count << std::endl;
                s_done = true;
                quit();
                return;
            }
        }

        struct UiSecondsTag;
        void onSetUISeconds(uint8_t *, R5900Context *ctx, PS2Runtime *)
        {
            if (s_clock)
            {
                const float stepped = s_clockBase + static_cast<float>(s_clockPolls) / 64.0f;
                ctx->f[12] = s_frozen ? std::min(stepped, s_uiSeconds) : stepped;
            }
            else if (s_frozen)
                ctx->f[12] = s_uiSeconds;
        }

        struct SecondsBeatTag;
        void onSetSecondsBeat(uint8_t *, R5900Context *ctx, PS2Runtime *)
        {
            if (!s_frozen)
                return;
            ctx->f[12] = s_seconds;
            ctx->f[13] = s_beat;
        }

        void onUiPoll(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (s_done || s_text.empty())
                return;
            s_lastPoll = Clock::now().time_since_epoch().count();
            s_runtime = runtime;
            s_rdram = rdram;
            ++s_clockPolls;
            if (transplant::active())
            {
                // The steps were in the memory replaced. What is left is the
                // frame it draws: a shot and a dump of it, then out.
                ++s_transplantPolls;
                if (s_transplantPolls == 3)
                    requestShot(s_transplantName);
                else if (s_transplantPolls > 3 && !shotPending() && !drawDumpPending())
                {
                    quit();
                }
                return;
            }
            const R5900Context saved = *ctx;
            if (s_steps == 0u)
            {
                s_start = now();
                s_steps = script::parse(rdram, ctx, runtime, s_text);
                if (s_steps == 0u)
                {
                    std::cerr << "[scenario] FAIL: parse error" << std::endl;
                    quit();
                }
            }
            if (s_steps != 0u)
                step(rdram, ctx, runtime);
            *ctx = saved;
        }

        // UIManager::Poll, which another run's memory must not go through.
        PS2Runtime::RecompiledFunction s_poll = nullptr;
        void uiPoll(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            onUiPoll(rdram, ctx, runtime);
            if (transplant::active())
                ctx->pc = GPR_U32(ctx, 31);
            else
                s_poll(rdram, ctx, runtime);
        }
    }

    bool load(const std::string &path)
    {
        s_text.clear();
        return read(path, s_text);
    }

    void install(PS2Runtime &runtime, const Addresses &addresses)
    {
        // {ghc_log ...}: each value and its type, to stderr. The game's own
        // print goes nowhere in a ship build.
        script::addCommand("ghc_log", [](const script::Call &call) -> script::Node {
            std::string line;
            for (int i = 1; i < call.size(); ++i)
            {
                const script::Node node = call.arg(i);
                line += (i > 1 ? " " : "") + text(call.rdram, node) + ":" + std::to_string(node.type);
            }
            std::cerr << "[script] " << line << std::endl;
            return {};
        });
        s_addresses = &addresses;
        s_poll = runtime.lookupFunction(addresses.uiManagerPoll);
        runtime.replaceFunction(addresses.uiManagerPoll, &uiPoll);
        EntryHook<UiSecondsTag>::install(runtime, addresses.taskMgrSetUISeconds, onSetUISeconds);
        EntryHook<SecondsBeatTag>::install(runtime, addresses.taskMgrSetSecondsBeat, onSetSecondsBeat);
        if (!s_text.empty())
            std::thread(watchdog).detach();
    }
}
