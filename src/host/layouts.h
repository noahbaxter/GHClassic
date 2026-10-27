#pragma once

// input.ini as this run read it, and the keyboard's layout from it. A
// controller's section is named by its SDL name and GUID; an empty name is
// the keyboard's. The host thread's own.

#include "host/bindings.h"

#include <functional>
#include <map>
#include <string>
#include <vector>

namespace gh2::input
{
    using Section = std::map<Action, std::vector<Source>>;

    // input.ini read, and written from then on. A run that never loads it
    // (a hidden, scripted one) keeps every change in memory: the player's
    // input.ini is the real controllers'.
    void loadLayouts();
    const BindingFile &layoutFile();

    // Its built-in layout with input.ini's section over it.
    const Profile &keyboardLayout();
    // A device's section: its GUID's, else one naming it alone; none if the
    // file has neither.
    const Section *sectionOf(const std::string &sdlName, const std::string &guid);
    // A device input.ini has no section for gets one, its built-in layout
    // as comments, so the file shows what there is to change.
    void listInFile(const std::string &sdlName, const std::string &guid, const Profile &builtin);
    // The section written with `change` made to what it had (an older file's
    // section by name alone moving to its GUID's), and read back; or with
    // `inMemory` kept to this run. A section with nothing of its own left is
    // its built-in layout again, as comment lines. False when the file could
    // not be written.
    bool editSection(const std::string &sdlName, const std::string &guid, const Profile &builtin, bool inMemory,
                     const std::function<void(Section &)> &change);
}
