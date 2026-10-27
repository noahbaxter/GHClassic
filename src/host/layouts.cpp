#include "host/layouts.h"

#include "host/profiles.h"

#include <iostream>

namespace gh2::input
{
    namespace
    {
        bool s_writable = false;
        BindingFile s_file;

        Profile &keyboard()
        {
            static Profile profile = keyboardProfile();
            return profile;
        }

        std::string nameOf(const std::string &sdlName, const std::string &guid)
        {
            return sdlName.empty() ? keyboardSection() : deviceSection(sdlName, guid);
        }

        const Section *named(const std::string &name)
        {
            const auto found = s_file.sections.find(name);
            return found == s_file.sections.end() ? nullptr : &found->second;
        }
    }

    void loadLayouts()
    {
        const std::string path = bindingFilePath();
        s_file = loadBindingFile(path);
        s_writable = true;
        keyboard() = overlay(keyboardProfile(), named(keyboardSection()));
        std::cerr << "[input] bindings: " << path << (s_file.sections.empty() ? " (none yet)" : "") << std::endl;
        listInFile("", "", keyboardProfile());
    }

    const BindingFile &layoutFile()
    {
        return s_file;
    }

    const Profile &keyboardLayout()
    {
        return keyboard();
    }

    const Section *sectionOf(const std::string &sdlName, const std::string &guid)
    {
        return sdlName.empty() ? named(keyboardSection()) : deviceBindings(s_file, sdlName, guid);
    }

    std::string sectionMode(const std::string &sdlName, const std::string &guid)
    {
        for (const std::string &name : {nameOf(sdlName, guid), deviceSection(sdlName)})
            if (named(name))
            {
                const auto mode = s_file.modes.find(name);
                return mode == s_file.modes.end() ? "" : mode->second;
            }
        return "";
    }

    DeviceKind kindOf(const std::string &sdlName, const std::string &guid, DeviceKind detected)
    {
        const std::string mode = sectionMode(sdlName, guid);
        return mode == "pad" ? DeviceKind::kGamepad : mode == "guitar" ? DeviceKind::kGuitar : detected;
    }

    void listInFile(const std::string &sdlName, const std::string &guid, const Profile &builtin)
    {
        const std::string name = nameOf(sdlName, guid);
        if (named(name))
            return;
        if (writeTemplate(bindingFilePath(), name, builtin))
            s_file.sections[name];
        else
            std::cerr << "[input] could not write " << bindingFilePath() << std::endl;
    }

    bool editSection(const std::string &sdlName, const std::string &guid, const Profile &builtin, bool inMemory,
                     const std::function<void(Section &, std::string &)> &change)
    {
        const std::string name = nameOf(sdlName, guid);
        const Section *own = sectionOf(sdlName, guid);
        Section lines = own ? *own : Section{};
        std::string mode = sectionMode(sdlName, guid);
        change(lines, mode);
        const std::string path = bindingFilePath();
        inMemory = inMemory || !s_writable;
        if (inMemory)
        {
            s_file.sections[name] = lines;
            if (mode.empty())
                s_file.modes.erase(name);
            else
                s_file.modes[name] = mode;
        }
        else if (!(lines.empty() ? writeTemplate(path, name, builtin, mode) : writeSection(path, name, lines, mode)))
        {
            std::cerr << "[input] could not write " << path << std::endl;
            return false;
        }
        else
            s_file = loadBindingFile(path);
        keyboard() = overlay(keyboardProfile(), named(keyboardSection()));
        std::cerr << "[input] [" << name << (inMemory ? "] set, in memory" : "] written") << std::endl;
        return true;
    }
}
