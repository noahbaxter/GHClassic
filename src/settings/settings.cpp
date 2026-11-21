#include "settings/settings.h"

#include "build_info.h"
#include "disc/disc.h"
#include "settings/ini.h"
#include "settings_template.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
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
            kText,   // any words, kept as they are; its value 0
        };

        // A setting's kind. Its default, section and place in the file, and
        // the words a player reads, are config/settings.template.ini's.
        struct Entry
        {
            const char *name;
            Type type;
            int min, max;
            int step;      // values snap to multiples of it
            int authentic; // what the PS2 did
            const char *choices = nullptr; // kChoice's words, space separated
        };

        std::vector<std::string> choices(const Entry &entry)
        {
            std::istringstream in(entry.choices ? entry.choices : "");
            return {std::istream_iterator<std::string>(in), {}};
        }

        const Entry kEntries[kKeyCount] = {
            {"widescreen", Type::kBool, 0, 1, 1, 0},
            {"fullscreen", Type::kBool, 0, 1, 1, 0},
            {"frame_rate_cap", Type::kInt, 0, 500, 1, 60},
            {"vsync", Type::kBool, 0, 1, 1, 1},
            // Most of GH2's textures have no mip chain (443 of 470 loaded into
            // a song), so drawn small they shimmer: the highway's far end.
            {"mipmaps", Type::kBool, 0, 1, 1, 0},
            {"msaa", Type::kInt, 1, 16, 1, 1},
            // A percent. 200 at most: the window takes the picture through one
            // linear blit, which is the mean of a 2x2 block at twice the size
            // and skips pixels past it.
            {"render_scale", Type::kInt, 50, 200, 25, 100},
            // Off: a sphere that holds only part of its mesh hides what still
            // shows (flashpot_4.mesh in battle's fail sequence).
            {"frustum_cull", Type::kBool, 0, 1, 1, 1},
            // 5 ms: finer is below what a player can feel. 0 by default, as
            // reset sets it: calibration is the player's own setup. A frame
            // reaches the screen two or three refreshes after it is drawn
            // (FIFO presentation, two frames in flight); Noah's Mac matched
            // at video 80 with audio 80, 2026-10-10.
            {"video_ms", Type::kInt, -500, 500, 5, 0},
            // 0 by default, as video. The port's own delay on macOS, SDL's
            // CoreAudio queue, is about 43 ms (the song clock is held to the
            // mixed audio, synth.cpp); not measured on Windows or Linux yet.
            {"audio_ms", Type::kInt, -500, 500, 5, 0},
            // OptionData's (retail 0x10d2e0), which the save carried.
            {"band_volume", Type::kInt, 0, 11, 1, 11},
            {"guitar_volume", Type::kInt, 0, 11, 1, 11},
            {"fx_volume", Type::kInt, 0, 11, 1, 11},
            {"stereo", Type::kBool, 0, 1, 1, 1},
            // Retail's practice slices the song to keep it, on its own
            // slowed files.
            {"maintain_pitch", Type::kBool, 0, 1, 1, 1},
            {"fast_boot", Type::kBool, 0, 1, 1, 0},
            {"lefty", Type::kBool, 0, 1, 1, 0},
            {"lefty_p2", Type::kBool, 0, 1, 1, 0},
            {"true_lefty_flip", Type::kBool, 0, 1, 1, 1},
            // Off: PCSX2 holds a card it has loaded open.
            {"export_card", Type::kBool, 0, 1, 1, 0},
            {"track_on_at_start", Type::kBool, 0, 1, 1, 0},
            // 100: the hit window (slop, beatmatcher.dta), so the note after
            // can be hit from when the release stops muting.
            {"sustain_release_ms", Type::kInt, 0, 500, 5, 0},
            {"miss_mute", Type::kBool, 0, 1, 1, 1},
            {"whammy_hold", Type::kBool, 0, 1, 1, 0},
            {"strum_limit", Type::kBool, 0, 1, 1, 1},
            {"star_power_overlap", Type::kBool, 0, 1, 1, 0},
            {"no_fail", Type::kBool, 0, 1, 1, 0},
            {"note_streak", Type::kBool, 0, 1, 1, 0},
            {"song_progress", Type::kBool, 0, 1, 1, 0},
            {"tap_rope", Type::kChoice, 0, 2, 1, kRopeRails, "off line rails"},
            // The unlock-all cheat's, without its turning off saving and the
            // career (content/campaigns.cpp).
            {"unlock_all", Type::kBool, 0, 1, 1, 0},
            // 70 is Clone Hero's 7.0. The defaults are retail's: 43 is easy and
            // medium's own speed and 60 hard and expert's; the hyperspeed cheat
            // made them 64 and 90 (note_speed.cpp). The menu's sliders stop
            // at 200, and the highway's too; the file takes each to 500, for
            // whoever goes looking.
            {"note_speed_easy", Type::kInt, 43, 500, 1, 43},
            {"note_speed_hard", Type::kInt, 43, 500, 1, 60},
            // horizon_y (track_graphics.dta): the PS2's 110, the 360's 120.
            {"highway_length", Type::kInt, 50, 500, 5, 110},
            // With both in PUT_DISC_HERE. A .chd is smaller; an .iso reads
            // faster, which only shows with the game run far above 1x.
            {"prefer", Type::kChoice, 0, 1, 1, kChd, "chd iso"},
            {"check_updates", Type::kBool, 0, 1, 1, 1},
            {"band", Type::kChoice, 0, 3, 1, kBandVenue, "venue gh2 gh1 gh2x"},
            {"depth_of_field", Type::kBool, 0, 1, 1, 1},
            {"gh1_band_shadows", Type::kChoice, 0, 2, 1, kShadowsGh2, "off classic gh2"},
            // Off: the pickers. Retail's quickplay goes from the difficulty
            // straight to the song.
            {"song_defaults", Type::kBool, 0, 1, 1, 1},
            {"in_song_ui", Type::kChoice, 0, 3, 1, kInSongVenue, "venue gh1 gh2 gh80s"},
            {"menu_music", Type::kChoice, 0, 4, 1, kMusicTheme, "theme gh1 gh2 gh80s all"},
            // Retail plays one loop over and over.
            {"menu_music_order", Type::kChoice, 0, 2, 1, kOrderLoop, "playlist loop shuffle"},
            // hd: the 360 disc's venues, characters and guitars, and its
            // track, each converted as its scene loads (gh2x/models.h).
            {"venues", Type::kChoice, 0, 1, 1, kSd, "sd hd"},
            {"characters", Type::kChoice, 0, 1, 1, kSd, "sd hd"},
            {"guitars", Type::kChoice, 0, 1, 1, kSd, "sd hd"},
            {"highway", Type::kChoice, 0, 2, 1, kHighwayClassic, "auto classic hd"},
            {"temporal_aa", Type::kBool, 0, 1, 1, 0},
            {"high_detail", Type::kBool, 0, 1, 1, 0},
            {"frame_log", Type::kBool, 0, 1, 1, 0},
            {"lighting", Type::kChoice, 0, 1, 1, kLastGen, "last_gen next_gen"},
            {"perf_overlay", Type::kChoice, 0, 3, 1, 0, "off fps graph full"},
            {"vector_fonts", Type::kBool, 0, 1, 1, 1},
            {"folders", Type::kText, 0, 0, 1, 0},
            {"show_duplicates", Type::kBool, 0, 1, 1, 0},
            {"disc_song_guitars", Type::kBool, 0, 1, 1, 0},
            {"practice_metronome", Type::kBool, 0, 1, 1, 0},
            {"practice_note_click", Type::kBool, 0, 1, 1, 0},
            {"practice_solo_stems", Type::kBool, 0, 1, 1, 1},
        };

        const Entry *entryNamed(const std::string &name)
        {
            const Entry *entry = std::find_if(std::begin(kEntries), std::end(kEntries),
                                              [&](const Entry &e) { return name == e.name; });
            return entry != std::end(kEntries) ? entry : nullptr;
        }

        // A `key = value  # note` line's key and value, or none for any other
        // line. A note's # follows a space, so a folder's (D:\#Charts) is
        // the value's.
        std::optional<std::pair<std::string, std::string>> assignment(const std::string &line)
        {
            const std::string trimmed = ini::trim(line);
            const size_t eq = trimmed.find('=');
            if (ini::skipped(trimmed) || ini::sectionName(trimmed) || eq == std::string::npos)
                return std::nullopt;
            size_t note = trimmed.find('#', eq);
            while (note != std::string::npos && trimmed[note - 1u] != ' ' && trimmed[note - 1u] != '\t')
                note = trimmed.find('#', note + 1u);
            return std::make_pair(ini::trim(trimmed.substr(0, eq)),
                                  ini::trim(trimmed.substr(eq + 1, note == std::string::npos ? std::string::npos : note - eq - 1)));
        }

        int snap(const Entry &entry, int value)
        {
            const int stepped = static_cast<int>(std::lround(static_cast<double>(value) / entry.step)) * entry.step;
            return std::clamp(stepped, entry.min, entry.max);
        }

        struct State
        {
            std::array<int, kKeyCount> value{};
            std::array<bool, kKeyCount> set{};
            std::array<std::string, kKeyCount> text; // kText's
        };

        std::string s_path;

        std::string path()
        {
            return s_path.empty() ? userDataPath("settings.ini") : s_path;
        }

        bool parse(const Entry &entry, const std::string &text, int &out)
        {
            if (entry.type == Type::kText)
            {
                out = 0;
                return true;
            }
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

        // The template's values are the defaults; settings.ini's over them,
        // each by its name whatever section it is under.
        State load()
        {
            State state;
            std::array<bool, kKeyCount> defaulted{};
            std::istringstream defaults(kSettingsTemplate);
            for (std::string line; std::getline(defaults, line);)
                if (const auto pair = assignment(line))
                {
                    const Entry *entry = entryNamed(pair->first);
                    int value = 0;
                    if (entry && parse(*entry, pair->second, value))
                    {
                        state.value[entry - kEntries] = value;
                        state.text[entry - kEntries] = entry->type == Type::kText ? pair->second : "";
                        defaulted[entry - kEntries] = true;
                    }
                    else
                        std::cerr << "[settings] template: ignored: " << line << std::endl;
                }
            for (int k = 0; k < kKeyCount; ++k)
                if (!defaulted[k])
                {
                    std::cerr << "[settings] template has no " << kEntries[k].name << std::endl;
                    state.value[k] = kEntries[k].min;
                }

            const std::string file = path();
            std::ifstream in(file);
            std::string line;
            int number = 0;
            while (std::getline(in, line))
            {
                ++number;
                const auto pair = assignment(line);
                if (!pair)
                    continue;
                const Entry *entry = entryNamed(pair->first);
                int value = 0;
                if (!entry || !parse(*entry, pair->second, value))
                {
                    std::cerr << "[settings] " << file << ":" << number << ": ignored: " << ini::trim(line) << std::endl;
                    continue;
                }
                state.value[entry - kEntries] = value;
                state.set[entry - kEntries] = true;
                if (entry->type == Type::kText)
                    state.text[entry - kEntries] = pair->second;
            }
            return state;
        }

        State &state()
        {
            static State s = load();
            return s;
        }

        std::string text(const Entry &e, int v)
        {
            return e.type == Type::kBool ? (v ? "true" : "false") : e.type == Type::kChoice ? choices(e)[v] : std::to_string(v);
        }

        // The template with each value as it is now, so the file lists what
        // can be set, in its sections, with its notes in their column.
        void save()
        {
            const std::string file = path();
            const std::error_code error = ini::writeReplacing(file, [](std::ofstream &out) {
                std::istringstream lines(kSettingsTemplate);
                for (std::string line; std::getline(lines, line);)
                {
                    const auto pair = assignment(line);
                    const Entry *entry = pair ? entryNamed(pair->first) : nullptr;
                    if (!entry)
                    {
                        out << line << "\n";
                        continue;
                    }
                    const size_t eq = line.find('='), note = line.find('#', eq);
                    std::string value = entry->type == Type::kText ? state().text[entry - kEntries]
                                                                   : text(*entry, state().value[entry - kEntries]);
                    if (note != std::string::npos)
                        value.resize(std::max(value.size() + 1u, note - eq - 2u), ' ');
                    out << line.substr(0, eq + 1) << " " << value
                        << (note != std::string::npos ? line.substr(note) : "") << "\n";
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

    std::string text(Key key)
    {
        return state().text[key];
    }

    namespace
    {
        std::function<Playing()> s_playing;
    }

    void setPlaying(std::function<Playing()> now) { s_playing = std::move(now); }

    bool hd(Look look)
    {
        const Playing playing = s_playing ? s_playing() : Playing{};
        if (!playing.mounted)
            return false;
        if (look == kLookUiArt)
            return true;
        if (look == kLookLighting && get(kLighting) == kLastGen)
            return false;
        if (playing.career)
            return playing.nextGen;
        const bool venue = playing.gh2Venue && (get(kVenues) == kHd || playing.nextGenSong);
        switch (look)
        {
        case kLookVenue:
            return venue;
        case kLookCharacters:
            return get(kCharacters) == kHd;
        case kLookGuitars:
            return get(kGuitars) == kHd || playing.nextGenSong;
        case kLookHighway:
            return get(kHighway) == kHighwayAuto ? venue : get(kHighway) == kHighwayHd;
        default:
            // The 360's shading on a PS2 room gets its exposure wrong, where
            // a PS2 model in a 360 room only looks flatter than its own.
            return playing.inVenue ? venue : get(kCharacters) == kHd || get(kGuitars) == kHd;
        }
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

    std::string besideSettings(const std::string &file)
    {
        return (std::filesystem::path(path()).parent_path() / file).string();
    }

    void set(Key key, int value)
    {
        const int snapped = snap(kEntries[key], value);
        if (snapped != state().value[key])
            std::cerr << "[settings] " << kEntries[key].name << " = " << text(kEntries[key], snapped) << std::endl;
        state().value[key] = snapped;
        state().set[key] = true;
        save();
    }

    std::string describe()
    {
        std::string out;
        for (int k = 0; k < kKeyCount; ++k)
        {
            const Entry &entry = kEntries[k];
            out += (k ? " " : "") + std::string(entry.name) + "=" +
                   (entry.type == Type::kText ? "\"" + state().text[k] + "\"" : text(entry, state().value[k]));
        }
        return out;
    }

    void rereadGraphics()
    {
        std::error_code error;
        const auto written = std::filesystem::last_write_time(path(), error);
        static auto seen = written;
        if (error || written == seen)
            return;
        seen = written;
        std::ifstream in(path());
        for (std::string line; std::getline(in, line);)
        {
            const auto pair = assignment(line);
            const Entry *entry = pair ? entryNamed(pair->first) : nullptr;
            if (!entry)
                continue;
            const Key key = static_cast<Key>(entry - kEntries);
            int value = 0;
            if ((key != kBand && (key < kVenues || key > kHighway)) || !parse(*entry, pair->second, value) ||
                value == state().value[key])
                continue;
            state().value[key] = value;
            state().set[key] = true;
            std::cerr << "[settings] " << entry->name << " now " << pair->second << std::endl;
        }
    }
}
