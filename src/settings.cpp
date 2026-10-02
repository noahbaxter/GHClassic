#include "settings.h"

#include "disc.h"
#include "ini.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace gh2::settings
{
    namespace
    {
        enum class Type
        {
            kBool,
            kInt,
        };

        struct Entry
        {
            const char *section;
            const char *name;
            Type type;
            int min, max;
            int step;      // values snap to multiples of it
            int modern;    // the default
            int authentic; // what the PS2 did
        };

        const Entry kEntries[kKeyCount] = {
            {"video", "widescreen", Type::kBool, 0, 1, 1, 1, 0},
            {"video", "frame_rate_cap", Type::kInt, 0, 500, 1, 0, 60},
            // Most of GH2's textures have no mip chain (443 of 470 loaded into
            // a song), so drawn small they shimmer: the highway's far end.
            {"video", "mipmaps", Type::kBool, 0, 1, 1, 1, 0},
            {"video", "msaa", Type::kInt, 1, 16, 1, 4, 1},
            // 5 ms: finer is below what a player can feel.
            {"latency", "video_ms", Type::kInt, -500, 500, 5, 0, 0},
            // 65 by default: the port's own delay, measured on macOS (the song
            // clock about 22 ms ahead of what is rendered, SDL's CoreAudio queue
            // about 43 ms). Not measured on Windows or Linux yet.
            {"latency", "audio_ms", Type::kInt, -500, 500, 5, 65, 0},
            {"game", "fast_boot", Type::kBool, 0, 1, 1, 1, 0},
        };

        int snap(const Entry &entry, int value)
        {
            const int stepped = static_cast<int>(std::lround(static_cast<double>(value) / entry.step)) * entry.step;
            return std::clamp(stepped, entry.min, entry.max);
        }

        struct State
        {
            std::array<int, kKeyCount> value{};
            std::array<bool, kKeyCount> set{};
        };

        std::string s_path;

        std::string path()
        {
            return s_path.empty() ? userDataPath("settings.ini") : s_path;
        }

        bool parse(const Entry &entry, const std::string &text, int &out)
        {
            if (entry.type == Type::kBool && (text == "true" || text == "false"))
            {
                out = text == "true";
                return true;
            }
            try
            {
                size_t used = 0;
                const int v = std::stoi(text, &used);
                if (used != text.size() || v < entry.min || v > entry.max)
                    return false;
                out = snap(entry, v);
                return true;
            }
            catch (const std::exception &)
            {
                return false;
            }
        }

        State load()
        {
            State state;
            for (int k = 0; k < kKeyCount; ++k)
                state.value[k] = kEntries[k].modern;

            const std::string file = path();
            std::ifstream in(file);
            std::string line, section;
            int number = 0;
            while (std::getline(in, line))
            {
                ++number;
                line = ini::trim(line);
                if (ini::skipped(line))
                    continue;
                if (const auto name = ini::sectionName(line))
                {
                    section = *name;
                    continue;
                }
                const size_t eq = line.find('=');
                const std::string name = eq == std::string::npos ? "" : ini::trim(line.substr(0, eq));
                const Entry *entry = std::find_if(std::begin(kEntries), std::end(kEntries), [&](const Entry &e) {
                    return section == e.section && name == e.name;
                });
                int value = 0;
                if (entry == std::end(kEntries) || !parse(*entry, ini::trim(line.substr(eq + 1)), value))
                {
                    std::cerr << "[settings] " << file << ":" << number << ": ignored: " << line << std::endl;
                    continue;
                }
                const size_t k = entry - kEntries;
                state.value[k] = value;
                state.set[k] = true;
            }
            return state;
        }

        State &state()
        {
            static State s = load();
            return s;
        }

        // Every entry, set or not, so the file lists what can be set.
        void save()
        {
            const std::string file = path();
            const std::error_code error = ini::writeReplacing(file, [](std::ofstream &out) {
                const char *section = "";
                for (int k = 0; k < kKeyCount; ++k)
                {
                    const Entry &e = kEntries[k];
                    if (std::string(section) != e.section)
                    {
                        out << (k ? "\n[" : "[") << e.section << "]\n";
                        section = e.section;
                    }
                    const int v = state().value[k];
                    out << e.name << " = " << (e.type == Type::kBool ? (v ? "true" : "false") : std::to_string(v))
                        << "\n";
                }
            });
            if (error)
                std::cerr << "[settings] could not write " << file << ": " << error.message() << std::endl;
        }
    }

    int get(Key key)
    {
        return state().value[key];
    }

    bool isSet(Key key)
    {
        return state().set[key];
    }

    void usePath(const std::string &path)
    {
        s_path = path;
    }

    // A portable install keeps it all beside the executable. Otherwise the
    // project was ghrecomp: its directory, when it is the only one, is moved
    // over whole the first time.
    std::string userDataPath(const std::string &file)
    {
        static const std::string dir = []
        {
            if (disc::portable())
                return std::string(SDL_GetBasePath());
            char *pref = SDL_GetPrefPath("", "GHClassic");
            if (!pref)
                return std::string();
            const std::string made = pref;
            SDL_free(pref);
            namespace fs = std::filesystem;
            std::error_code error;
            const fs::path current = fs::path(made).parent_path();
            const fs::path old = current.parent_path() / "ghrecomp";
            if (fs::is_directory(old, error) && fs::is_empty(current, error))
            {
                fs::remove(current, error);
                fs::rename(old, current, error);
                if (error)
                {
                    std::cerr << "[settings] could not move " << old << ": " << error.message() << std::endl;
                    fs::create_directories(current, error);
                }
                else
                    std::cerr << "[settings] moved " << old << " to " << current << std::endl;
            }
            return made;
        }();
        return dir + file;
    }

    void set(Key key, int value)
    {
        state().value[key] = snap(kEntries[key], value);
        state().set[key] = true;
        save();
    }
}
