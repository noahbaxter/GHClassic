#include "settings/settings.h"

#include "build_info.h"
#include "disc/disc.h"
#include "settings/ini.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <sstream>
#include <string>
#include <vector>

namespace gh2::settings
{
    namespace
    {
        enum class Type
        {
            kBool,
            kInt,
            kChoice, // one of `choices`, its value the word's position
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
            const char *choices = nullptr; // kChoice's words, space separated
        };

        std::vector<std::string> choices(const Entry &entry)
        {
            std::istringstream in(entry.choices ? entry.choices : "");
            return {std::istream_iterator<std::string>(in), {}};
        }

        const Entry kEntries[kKeyCount] = {
            {"video", "widescreen", Type::kBool, 0, 1, 1, 1, 0},
            {"video", "frame_rate_cap", Type::kInt, 0, 500, 1, 0, 60},
            // Most of GH2's textures have no mip chain (443 of 470 loaded into
            // a song), so drawn small they shimmer: the highway's far end.
            {"video", "mipmaps", Type::kBool, 0, 1, 1, 1, 0},
            {"video", "msaa", Type::kInt, 1, 16, 1, 4, 1},
            // Off: a sphere that holds only part of its mesh hides what still
            // shows (flashpot_4.mesh in battle's fail sequence).
            {"video", "frustum_cull", Type::kBool, 0, 1, 1, 0, 1},
            // 5 ms: finer is below what a player can feel.
            {"latency", "video_ms", Type::kInt, -500, 500, 5, 0, 0},
            // 45 by default: the port's own delay on macOS, SDL's CoreAudio
            // queue (about 43 ms; the song clock is held to the mixed audio,
            // synth.cpp). Not measured on Windows or Linux yet.
            {"latency", "audio_ms", Type::kInt, -500, 500, 5, 45, 0},
            // OptionData's (retail 0x10d2e0), which the save carried.
            {"audio", "band_volume", Type::kInt, 0, 11, 1, 11, 11},
            {"audio", "guitar_volume", Type::kInt, 0, 11, 1, 11, 11},
            {"audio", "fx_volume", Type::kInt, 0, 11, 1, 11, 11},
            {"audio", "stereo", Type::kBool, 0, 1, 1, 1, 1},
            {"game", "fast_boot", Type::kBool, 0, 1, 1, 1, 0},
            {"game", "lefty", Type::kBool, 0, 1, 1, 0, 0},
            {"game", "lefty_p2", Type::kBool, 0, 1, 1, 0, 0},
            // Off: PCSX2 holds a card it has loaded open.
            {"save", "export_card", Type::kBool, 0, 1, 1, 0, 0},
            {"game", "track_on_at_start", Type::kBool, 0, 1, 1, 1, 0},
            // 100: the hit window (slop, beatmatcher.dta), so the note after
            // can be hit from when the release stops muting.
            {"game", "sustain_release_ms", Type::kInt, 0, 500, 5, 100, 0},
            {"game", "whammy_hold", Type::kBool, 0, 1, 1, 1, 0},
            // With both in PUT_DISC_HERE. A .chd is smaller; an .iso reads
            // faster, which only shows with the game run far above 1x.
            {"disc", "prefer", Type::kChoice, 0, 1, 1, kChd, kChd, "chd iso"},
            {"game", "check_updates", Type::kBool, 0, 1, 1, 1, 1},
            {"game", "band", Type::kChoice, 0, 2, 1, kBandVenue, kBandVenue, "venue gh2 gh1"},
            {"video", "depth_of_field", Type::kBool, 0, 1, 1, 1, 1},
            {"video", "gh1_band_shadows", Type::kChoice, 0, 2, 1, kShadowsGh2, kShadowsGh2, "off classic gh2"},
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
            if (entry.type == Type::kChoice)
            {
                const std::vector<std::string> words = choices(entry);
                const auto word = std::find(words.begin(), words.end(), text);
                out = static_cast<int>(word - words.begin());
                return word != words.end();
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
                    out << e.name << " = "
                        << (e.type == Type::kBool     ? (v ? "true" : "false")
                            : e.type == Type::kChoice ? choices(e)[v]
                                                      : std::to_string(v))
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

    namespace
    {
        std::string s_dataDir;
    }

    void useDataDir(const std::string &dir)
    {
        s_dataDir = dir;
    }

    // A portable install keeps it all beside the executable. Otherwise each
    // track has a directory of its own. The project was ghrecomp: its
    // directory, when it is the only one, is moved over whole the first time.
    std::string userDataPath(const std::string &file)
    {
        static const std::string dir = []
        {
            if (!s_dataDir.empty() && s_dataDir != "stable")
            {
                std::error_code error;
                std::filesystem::create_directories(s_dataDir, error);
                return (std::filesystem::path(s_dataDir) / "").string();
            }
            if (s_dataDir.empty() && disc::portable())
                return std::string(SDL_GetBasePath());
            const bool own = build::kExperimental && s_dataDir.empty();
            char *pref = SDL_GetPrefPath("", own ? "GHClassicExperimental" : "GHClassic");
            if (!pref)
                return std::string();
            const std::string made = pref;
            SDL_free(pref);
            namespace fs = std::filesystem;
            std::error_code error;
            const fs::path current = fs::path(made).parent_path();
            const fs::path old = current.parent_path() / "ghrecomp";
            if (!build::kExperimental && fs::is_directory(old, error) && fs::is_empty(current, error))
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
