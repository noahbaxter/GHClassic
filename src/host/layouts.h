#pragma once

// input.ini as this run read it, and the keyboard's layout from it. A
// controller's section is named by its SDL name; an empty name is the
// keyboard's. The host thread's own.

#include "host/bindings.h"

#include <map>
#include <string>
#include <vector>

namespace gh2::input
{
    using Section = std::map<Action, std::vector<Source>>;

    void loadLayouts();
    const BindingFile &layoutFile();

    // Its built-in layout with input.ini's section over it.
    const Profile &keyboardLayout();
    // A device's section; none if the file has none.
    const Section *sectionOf(const std::string &sdlName);
    // A device input.ini has no section for gets one, its built-in layout
    // as comments, so the file shows what there is to change.
    void listInFile(const std::string &sdlName, const Profile &builtin);
}
