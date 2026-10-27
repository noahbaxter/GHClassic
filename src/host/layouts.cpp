#include "host/layouts.h"

#include "host/profiles.h"

#include <iostream>

namespace gh2::input
{
    namespace
    {
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
}
