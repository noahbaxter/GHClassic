#include "host/bindings.h"

#include "settings/ini.h"
#include "settings/settings.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <sstream>

namespace gh2::input
{
    namespace
    {
        const char *const kActionKeys[kActionCount] = {
            "green", "red", "yellow", "blue", "orange", "strum_up", "strum_down",
            "start", "star_power", "tilt", "whammy",
        };

        struct HatName
        {
            const char *name;
            uint8_t bit;
        };
        const HatName kHatNames[] = {
            {"up", SDL_HAT_UP}, {"right", SDL_HAT_RIGHT}, {"down", SDL_HAT_DOWN}, {"left", SDL_HAT_LEFT},
        };

        using ini::trim;

        std::vector<std::string> splitCommas(const std::string &s)
        {
            std::vector<std::string> out;
            std::stringstream in(s);
            std::string part;
            while (std::getline(in, part, ','))
                if (!trim(part).empty())
                    out.push_back(trim(part));
            return out;
        }

        bool parseAxisRange(std::istringstream &in, Source &s)
        {
            return static_cast<bool>(in >> s.rest >> s.full) && s.rest != s.full;
        }

        std::string formatFloat(float v)
        {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%.2f", v);
            return buf;
        }

        // SDL names the comma key ",", which the list would split on.
        constexpr const char *kCommaName = "comma";

        // A UTF-8 byte order mark, which some editors put before the first line.
        std::string withoutBom(std::string line)
        {
            if (line.rfind("\xEF\xBB\xBF", 0) == 0)
                line.erase(0, 3);
            return line;
        }

        // The file key for an action: "green", "strum_up", ...
        const char *actionKey(Action action)
        {
            return action < kActionCount ? kActionKeys[action] : "";
        }

        std::optional<Action> actionFromKey(const std::string &key)
        {
            for (int a = 0; a < kActionCount; ++a)
                if (key == kActionKeys[a])
                    return static_cast<Action>(a);
            return std::nullopt;
        }
    }

    std::optional<Source> parseSource(const std::string &text)
    {
        std::istringstream in(text);
        std::string kind;
        in >> kind;
        Source s;
        if (kind == "key")
        {
            std::string name;
            std::getline(in, name);
            name = trim(name);
            const SDL_Scancode code =
                name == kCommaName ? SDL_SCANCODE_COMMA : SDL_GetScancodeFromName(name.c_str());
            if (code == SDL_SCANCODE_UNKNOWN)
                return std::nullopt;
            s.kind = Source::kKey;
            s.index = code;
            return s;
        }
        if (kind == "button")
        {
            s.kind = Source::kButton;
            if (!(in >> s.index) || s.index < 0)
                return std::nullopt;
            return s;
        }
        if (kind == "hat")
        {
            std::string dir;
            if (!(in >> s.index >> dir) || s.index < 0)
                return std::nullopt;
            for (const HatName &h : kHatNames)
                if (dir == h.name)
                    s.hat = h.bit;
            if (s.hat == 0)
                return std::nullopt;
            s.kind = Source::kHat;
            return s;
        }
        if (kind == "axis")
        {
            s.kind = Source::kAxis;
            if (!(in >> s.index) || s.index < 0 || !parseAxisRange(in, s))
                return std::nullopt;
            return s;
        }
        if (kind == "pad")
        {
            std::string name;
            if (!(in >> name))
                return std::nullopt;
            if (name == "axis")
            {
                std::string axis;
                if (!(in >> axis))
                    return std::nullopt;
                s.kind = Source::kPadAxis;
                s.index = SDL_GetGamepadAxisFromString(axis.c_str());
                if (s.index == SDL_GAMEPAD_AXIS_INVALID || !parseAxisRange(in, s))
                    return std::nullopt;
                return s;
            }
            s.kind = Source::kPadButton;
            s.index = SDL_GetGamepadButtonFromString(name.c_str());
            if (s.index == SDL_GAMEPAD_BUTTON_INVALID)
                return std::nullopt;
            return s;
        }
        return std::nullopt;
    }

    std::string formatSource(const Source &s)
    {
        switch (s.kind)
        {
        case Source::kKey:
            return std::string("key ") +
                   (s.index == SDL_SCANCODE_COMMA ? kCommaName : SDL_GetScancodeName(static_cast<SDL_Scancode>(s.index)));
        case Source::kButton:
            return "button " + std::to_string(s.index);
        case Source::kHat:
            for (const HatName &h : kHatNames)
                if (s.hat == h.bit)
                    return "hat " + std::to_string(s.index) + " " + h.name;
            return "";
        case Source::kAxis:
            return "axis " + std::to_string(s.index) + " " + formatFloat(s.rest) + " " + formatFloat(s.full);
        case Source::kPadButton:
            return std::string("pad ") + SDL_GetGamepadStringForButton(static_cast<SDL_GamepadButton>(s.index));
        case Source::kPadAxis:
            return std::string("pad axis ") + SDL_GetGamepadStringForAxis(static_cast<SDL_GamepadAxis>(s.index)) +
                   " " + formatFloat(s.rest) + " " + formatFloat(s.full);
        }
        return "";
    }

    std::string keyboardSection()
    {
        return "keyboard";
    }

    std::string deviceSection(const std::string &name)
    {
        return "device \"" + name + "\"";
    }

    BindingFile loadBindingFile(const std::string &path)
    {
        BindingFile file;
        std::ifstream in(path);
        std::string line;
        std::string section;
        int number = 0;
        while (std::getline(in, line))
        {
            ++number;
            // Comments take whole lines: ';' and '#' are key names too.
            const std::string text = trim(number == 1 ? withoutBom(line) : line);
            if (ini::skipped(text))
                continue;
            if (const auto name = ini::sectionName(text))
            {
                section = *name;
                file.sections[section];
                continue;
            }
            const size_t eq = text.find('=');
            const auto action = eq == std::string::npos ? std::nullopt : actionFromKey(trim(text.substr(0, eq)));
            if (section.empty() || !action)
            {
                std::cerr << "[input] " << path << ":" << number << ": not a binding, skipped" << std::endl;
                continue;
            }
            // One control it cannot read skips the line, so the action keeps
            // what it had rather than losing the rest.
            std::vector<Source> sources;
            bool readable = true;
            for (const std::string &part : splitCommas(text.substr(eq + 1)))
            {
                if (const auto source = parseSource(part))
                    sources.push_back(*source);
                else
                {
                    std::cerr << "[input] " << path << ":" << number << ": \"" << part
                              << "\" is not a control, line skipped" << std::endl;
                    readable = false;
                }
            }
            if (readable)
                file.sections[section][*action] = std::move(sources);
        }
        return file;
    }

    bool writeSection(const std::string &path, const std::string &section,
                      const std::map<Action, std::vector<Source>> &bindings)
    {
        // Every line outside the section is kept as it was, comments included.
        std::vector<std::string> kept;
        {
            std::ifstream in(path);
            std::string line;
            bool inside = false;
            bool first = true;
            while (std::getline(in, line))
            {
                if (first)
                    line = withoutBom(line);
                first = false;
                if (const auto name = ini::sectionName(trim(line)))
                    inside = *name == section;
                if (!inside)
                    kept.push_back(line);
            }
        }
        while (!kept.empty() && trim(kept.back()).empty())
            kept.pop_back();

        return !ini::writeReplacing(path, [&](std::ofstream &out) {
            for (const std::string &line : kept)
                out << line << "\n";
            if (!kept.empty())
                out << "\n";
            out << "[" << section << "]\n";
            for (const auto &[action, sources] : bindings)
            {
                out << actionKey(action) << " =";
                for (size_t i = 0; i < sources.size(); ++i)
                    out << (i == 0 ? " " : ", ") << formatSource(sources[i]);
                out << "\n";
            }
        });
    }

    Profile overlay(Profile base, const std::map<Action, std::vector<Source>> *section)
    {
        if (section)
            for (const auto &[action, sources] : *section)
                base[action] = sources;
        return base;
    }

    std::string bindingFilePath()
    {
        return settings::userDataPath("input.ini");
    }
}
