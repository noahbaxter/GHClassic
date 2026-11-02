#pragma once

// input.ini as this run read it, and the keyboard's layout from it. A
// controller's section is named by its SDL name and GUID; an empty name is
// the keyboard's. The host thread's own.

#include "host/bindings.h"
#include "host/device_id.h"

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
    // That section's mode line, pad or guitar, "" for none; and the kind a
    // device plays as, `detected` unless the line says otherwise.
    std::string sectionMode(const std::string &sdlName, const std::string &guid);
    DeviceKind kindOf(const std::string &sdlName, const std::string &guid, DeviceKind detected);
    // The keyboard's mode line says pad: with no controller in P1's seat it
    // plays songs as a DualShock does, a fret press strumming.
    bool keyboardPlaysPad();
    // What a menu reads: [menu keyboard] or [menu gamepad] over the fixed
    // controls (menu_layer.h), the keyboard's keys only and a gamepad's
    // controls only; tilt and whammy move nothing. A line with none of its
    // kind keeps the default, as an empty one unbinds.
    const Profile &menuLayout(bool keyboard);
    Profile menuWith(bool keyboard, const Section *own);
    // One action of it set for this run alone, a scenario's.
    void setMenuBinding(bool keyboard, Action action, const std::vector<Source> &sources);
    // A device input.ini has no section for gets one, its built-in layout
    // as comments, so the file shows what there is to change.
    void listInFile(const std::string &sdlName, const std::string &guid, const Profile &builtin);
    // The section written with `change` made to what it had and its mode
    // (an older file's section by name alone moving to its GUID's), and read
    // back; or with `inMemory` kept to this run. A section with no bindings
    // of its own left is its built-in layout again, as comment lines. False
    // when the file could not be written.
    bool editSection(const std::string &sdlName, const std::string &guid, const Profile &builtin, bool inMemory,
                     const std::function<void(Section &, std::string &mode)> &change);
}
