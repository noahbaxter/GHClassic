#include "host/log.h"

#include "build_info.h"
#include "dev/timing.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <mutex>
#include <streambuf>
#include <string_view>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <io.h>
#include <windows.h>
#include <psapi.h>
#else
#include <csignal>
#include <execinfo.h>
#include <sys/utsname.h>
#include <unistd.h>
#endif
#ifdef __APPLE__
#include <mach/mach.h>
#include <sys/sysctl.h>
#endif

namespace gh2::log
{
    namespace
    {
        // Every run is appended. A launch drops the oldest whole runs past
        // kKeep, and a run stops writing past kRunCap (one that loops on a
        // warning), so the file stays under Discord's 10 MB upload.
        constexpr size_t kKeep = 5u << 20;
        constexpr size_t kRunCap = 3u << 20;
        constexpr const char *kClosed = "[log] closed";

        bool s_toFile = false;
        bool s_verbose = true;
        std::chrono::steady_clock::time_point s_start;
        std::mutex s_mutex;
        size_t s_written = 0;
        bool s_capped = false;
        char s_screen[64] = "boot";

        // Minutes, seconds and milliseconds since the start, hours past one.
        std::string stamp()
        {
            const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() -
                                                                                  s_start)
                                .count();
            char text[24];
            const long long s = ms / 1000;
            if (s >= 3600)
                std::snprintf(text, sizeof text, "[%lld:%02lld:%02lld.%03lld] ", s / 3600, s / 60 % 60, s % 60,
                              ms % 1000);
            else
                std::snprintf(text, sizeof text, "[%02lld:%02lld.%03lld] ", s / 60, s % 60, ms % 1000);
            return text;
        }

        // The home folder, each way its separators are written; every line
        // has it as ~.
        std::string s_home[2];

        void unhome(std::string &line)
        {
            for (const std::string &home : s_home)
                for (size_t at = home.empty() ? std::string::npos : line.find(home); at != std::string::npos;
                     at = line.find(home, at + 1))
                {
                    // C:\Users\al is not C:\Users\alex's.
                    const char next = at + home.size() < line.size() ? line[at + home.size()] : '\0';
                    if (next == '\\' || next == '/' || next == '\0' || next == '\n' || next == '"' || next == '\'' ||
                        next == ' ' || next == ')' || next == ',')
                        line.replace(at, home.size(), "~");
                }
        }

        void write(const std::string &line, bool always = false)
        {
            std::lock_guard lock(s_mutex);
            if (s_capped && !always)
                return;
            // PS2Recomp's, every boot: the IOP modules GH2 loads that the
            // host stands in for. A developer's line, not one of ours to keep
            // behind verbose().
            if (!s_verbose && line.find("[IOP:load-failed]") != std::string::npos)
                return;
            std::string out = stamp() + line;
            unhome(out);
            std::fwrite(out.data(), 1, out.size(), stderr);
            s_written += out.size();
            if (s_written > kRunCap && !s_capped)
            {
                s_capped = true;
                const std::string note = stamp() + "[log] " + std::to_string(kRunCap >> 20) +
                                         " MB written; the rest of this run is left out\n";
                std::fwrite(note.data(), 1, note.size(), stderr);
            }
            std::fflush(stderr);
        }

        // Each thread's line until its newline, so lines from two threads
        // never land inside each other.
        class Lines : public std::streambuf
        {
        protected:
            int overflow(int c) override
            {
                if (c == traits_type::eof())
                    return 0;
                const char ch = static_cast<char>(c);
                xsputn(&ch, 1);
                return c;
            }

            std::streamsize xsputn(const char *s, std::streamsize n) override
            {
                thread_local std::string line;
                const char *end = s + n;
                while (s < end)
                {
                    const char *newline = std::find(s, end, '\n');
                    if (newline == end)
                    {
                        line.append(s, end);
                        break;
                    }
                    line.append(s, newline + 1);
                    write(line);
                    line.clear();
                    s = newline + 1;
                }
                return n;
            }
        };
        Lines s_lines;

        // A run's first line is its [system] line, right after the stamp.
        bool startsRun(std::string_view line)
        {
            const size_t at = line.find("] ");
            return at != std::string_view::npos && line.substr(at + 2).rfind("[system] ", 0) == 0;
        }

        // The log's oldest whole runs dropped until the rest fits kKeep.
        // Whether its last run closed, for a line saying it did not.
        bool trim(const std::filesystem::path &path)
        {
            std::string text;
            {
                std::ifstream in(path, std::ios::binary);
                text.assign(std::istreambuf_iterator<char>(in), {});
            }
            const size_t last = text.rfind("] [system] ");
            const bool closed = last == std::string::npos || text.find(kClosed, last) != std::string::npos;
            if (text.size() <= kKeep)
                return closed;
            size_t from = text.size();
            for (size_t line = 0; line < text.size();)
            {
                const size_t end = std::min(text.find('\n', line), text.size() - 1u) + 1u;
                if (text.size() - line <= kKeep && startsRun(std::string_view(text).substr(line, end - line)))
                {
                    from = line;
                    break;
                }
                line = end;
            }
            // One run bigger than all we keep: its end, from a whole line.
            if (from == text.size())
            {
                const size_t newline = text.find('\n', text.size() - kKeep);
                from = newline == std::string::npos ? text.size() - kKeep : newline + 1u;
            }
            std::ofstream(path, std::ios::binary | std::ios::trunc).write(text.data() + from, text.size() - from);
            return closed;
        }

        // A crash's line: what failed, where, and on what screen. Written
        // past the stream's lock, which the crashing thread may hold.
        void crashLine(const char *what)
        {
            char line[512];
            int n = std::snprintf(line, sizeof line, "%s[crash] %s, on screen %s", stamp().c_str(), what, s_screen);
            if (const float song = timing::songSeconds(); song > 0.0f)
                n += std::snprintf(line + n, sizeof line - n, ", song at %.1f s", song);
            n += std::snprintf(line + n, sizeof line - n, "\n");
#ifdef _WIN32
            _write(_fileno(stderr), line, static_cast<unsigned>(std::max(n, 0)));
#else
            ::write(STDERR_FILENO, line, static_cast<size_t>(std::max(n, 0)));
#endif
        }

#ifdef _WIN32
        LONG WINAPI onCrash(EXCEPTION_POINTERS *info)
        {
            const EXCEPTION_RECORD &record = *info->ExceptionRecord;
            char what[384];
            char module[MAX_PATH] = "?";
            HMODULE handle = nullptr;
            uintptr_t offset = reinterpret_cast<uintptr_t>(record.ExceptionAddress);
            if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                   static_cast<LPCSTR>(record.ExceptionAddress), &handle))
            {
                GetModuleFileNameA(handle, module, sizeof module);
                offset -= reinterpret_cast<uintptr_t>(handle);
            }
            const char *name = std::strrchr(module, '\\');
            std::snprintf(what, sizeof what, "exception 0x%08lX at %s+0x%llx", record.ExceptionCode,
                          name ? name + 1 : module, static_cast<unsigned long long>(offset));
            if (record.ExceptionCode == EXCEPTION_ACCESS_VIOLATION && record.NumberParameters >= 2)
                std::snprintf(what + std::strlen(what), sizeof what - std::strlen(what), " (%s 0x%llx)",
                              record.ExceptionInformation[0] == 1 ? "writing" : "reading",
                              static_cast<unsigned long long>(record.ExceptionInformation[1]));
            crashLine(what);
            return EXCEPTION_CONTINUE_SEARCH;
        }
#else
        void onCrash(int signal)
        {
            char what[64];
            std::snprintf(what, sizeof what, "signal %d (%s)", signal, strsignal(signal));
            crashLine(what);
            void *frames[48];
            backtrace_symbols_fd(frames, backtrace(frames, 48), STDERR_FILENO);
            std::signal(signal, SIG_DFL);
            std::raise(signal);
        }
#endif
    }

    void open(const std::string &path)
    {
        s_start = std::chrono::steady_clock::now();
        const std::filesystem::path log = path;
        std::error_code error;
        // 0.9.1 kept the run before here; it is in the log itself now.
        std::filesystem::remove(std::filesystem::path(log).replace_extension(".prev.log"), error);
        const bool any = std::filesystem::file_size(log, error) > 0u && !error;
        const bool closed = !any || trim(log);
        if (!std::freopen(path.c_str(), "a", stderr))
            return;
#ifdef _WIN32
        _dup2(_fileno(stderr), _fileno(stdout));
#else
        dup2(fileno(stderr), fileno(stdout));
#endif
        std::setvbuf(stderr, nullptr, _IONBF, 0);
        s_toFile = true;
        s_verbose = false;
#ifdef _WIN32
        const char *home = std::getenv("USERPROFILE");
#else
        const char *home = std::getenv("HOME");
#endif
        if (home && std::strlen(home) > 3)
        {
            s_home[0] = home;
            s_home[1] = home;
            std::replace(s_home[1].begin(), s_home[1].end(), '\\', '/');
            if (s_home[1] == s_home[0])
                s_home[1].clear();
        }
        std::cerr.rdbuf(&s_lines);
        std::cout.rdbuf(&s_lines);
#ifdef _WIN32
        SetUnhandledExceptionFilter(onCrash);
#else
        for (int signal : {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT})
            std::signal(signal, onCrash);
#endif
        if (!closed)
            std::cerr << "[log] the run above never closed (a crash, a freeze or a kill)\n";
        // A blank line between runs.
        if (any)
            std::fputc('\n', stderr);
    }

    bool toFile() { return s_toFile; }

    void system()
    {
        std::string os = SDL_GetPlatform(), cpu;
#ifdef _WIN32
        // GetVersionEx says 6.2 to programs without a manifest naming 10.
        if (HMODULE ntdll = GetModuleHandleA("ntdll.dll"))
        {
            using RtlGetVersion = LONG(WINAPI *)(OSVERSIONINFOW *);
            OSVERSIONINFOW version{};
            version.dwOSVersionInfoSize = sizeof version;
            if (auto get = reinterpret_cast<RtlGetVersion>(GetProcAddress(ntdll, "RtlGetVersion")); get && get(&version) == 0)
                os += " " + std::to_string(version.dwMajorVersion) + "." + std::to_string(version.dwMinorVersion) + "." +
                      std::to_string(version.dwBuildNumber);
            using WineVersion = const char *(*)();
            if (auto wine = reinterpret_cast<WineVersion>(GetProcAddress(ntdll, "wine_get_version")))
                os += std::string(" under Wine ") + wine();
        }
        HKEY key;
        if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", 0, KEY_READ, &key) ==
            ERROR_SUCCESS)
        {
            char name[128] = "";
            DWORD size = sizeof name - 1;
            if (RegQueryValueExA(key, "ProcessorNameString", nullptr, nullptr, reinterpret_cast<BYTE *>(name), &size) ==
                ERROR_SUCCESS)
                cpu = name;
            RegCloseKey(key);
        }
#elif defined(__APPLE__)
        char text[128];
        size_t size = sizeof text;
        if (sysctlbyname("kern.osproductversion", text, &size, nullptr, 0) == 0)
            os += std::string(" ") + text;
        size = sizeof text;
        if (sysctlbyname("machdep.cpu.brand_string", text, &size, nullptr, 0) == 0)
            cpu = text;
#else
        const auto field = [](const char *path, const std::string &key) {
            std::ifstream in(path);
            for (std::string line; std::getline(in, line);)
                if (line.rfind(key, 0) == 0)
                {
                    std::string value = line.substr(std::min(line.find_first_not_of(" \t:=", key.size()), line.size()));
                    value.erase(std::remove(value.begin(), value.end(), '"'), value.end());
                    return value;
                }
            return std::string();
        };
        if (const std::string pretty = field("/etc/os-release", "PRETTY_NAME"); !pretty.empty())
            os = pretty;
        utsname uts{};
        if (uname(&uts) == 0)
            os += std::string(", kernel ") + uts.release;
        cpu = field("/proc/cpuinfo", "model name");
#endif
        // Windows pads the name out with spaces, older Intel ones in front.
        cpu.erase(0, std::min(cpu.find_first_not_of(' '), cpu.size()));
        cpu.erase(cpu.find_last_not_of(' ') + 1);
        // The player's languages, first preferred first, and on Windows the
        // code page text without Unicode is read in.
        std::string language;
        int count = 0;
        if (SDL_Locale **locales = SDL_GetPreferredLocales(&count))
        {
            for (int i = 0; i < count && i < 3; ++i)
                language += std::string(i ? " " : "") + locales[i]->language +
                            (locales[i]->country ? std::string("_") + locales[i]->country : "");
            SDL_free(locales);
        }
#ifdef _WIN32
        language += (language.empty() ? "" : ", ") + std::string("code page ") + std::to_string(GetACP());
#endif
        const std::time_t now = std::time(nullptr);
        char started[32];
        std::strftime(started, sizeof started, "%Y-%m-%d %H:%M", std::localtime(&now));
        std::cerr << "[system] GH Classic " << build::kVersion << (build::kExperimental ? " experimental" : "") << ", "
                  << os << ", " << (language.empty() ? "language ?" : language) << ", "
                  << (cpu.empty() ? "CPU ?" : cpu) << ", " << SDL_GetNumLogicalCPUCores() << " threads, "
                  << (SDL_GetSystemRAM() + 512) / 1024 << " GB, started " << started << std::endl;
    }

    uint64_t memoryBytes()
    {
#ifdef _WIN32
        PROCESS_MEMORY_COUNTERS counters{};
        counters.cb = sizeof counters;
        return K32GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof counters) ? counters.WorkingSetSize : 0u;
#elif defined(__APPLE__)
        mach_task_basic_info info{};
        mach_msg_type_number_t size = MACH_TASK_BASIC_INFO_COUNT;
        return task_info(mach_task_self(), MACH_TASK_BASIC_INFO, reinterpret_cast<task_info_t>(&info), &size) ==
                       KERN_SUCCESS
                   ? info.resident_size
                   : 0u;
#else
        // statm's second field: resident pages. No allocation, for the
        // song's end line.
        FILE *statm = std::fopen("/proc/self/statm", "r");
        unsigned long long pages = 0, resident = 0;
        if (statm)
        {
            if (std::fscanf(statm, "%llu %llu", &pages, &resident) != 2)
                resident = 0;
            std::fclose(statm);
        }
        return resident * static_cast<uint64_t>(sysconf(_SC_PAGESIZE));
#endif
    }

    void setVerbose() { s_verbose = true; }

    bool verbose() { return s_verbose; }

    void screen(const char *name)
    {
        std::cerr << "[screen] " << name << std::endl;
        std::snprintf(s_screen, sizeof s_screen, "%s", name);
    }

    void closed()
    {
        static std::atomic<bool> once{false};
        if (!s_toFile || once.exchange(true))
            return;
        std::cout.flush();
        write(std::string(kClosed) + "\n", true);
    }
}
