#include "scenario.h"

#include "guest.h"
#include "hook.h"
#include "host/vulkan_frontend.h"
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
        std::string s_screen;         // a wait_screen's screen
        uint32_t s_condition = 0u;    // a wait_until's step array
        bool s_shooting = false;      // a shot's frame not yet written
        Clock::time_point s_deadline; // and when either fails
        int s_timeout = kTimeout;     // that deadline's seconds, to report
        std::atomic<bool> s_done{false};

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
                if (shotPending())
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
                else if (verb == "shot")
                {
                    // Nothing more runs until that frame is written.
                    requestShot(array.symbol(1));
                    s_shooting = true;
                    return;
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
                for (const EeSemaphoreSnapshot &s : snapshot.semaphores)
                    std::cerr << "[scenario]   sema " << s.id << " count " << s.count << std::endl;
                s_done = true;
                quit();
                return;
            }
        }

        struct PollTag;
        void onUiPoll(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (s_done || s_text.empty())
                return;
            s_lastPoll = Clock::now().time_since_epoch().count();
            s_runtime = runtime;
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
        EntryHook<PollTag>::install(runtime, addresses.uiManagerPoll, onUiPoll);
        if (!s_text.empty())
            std::thread(watchdog).detach();
    }
}
