// GH1's menus in GH2's.
//
// GH1 keeps a screen as a flat scene, ghui/<screen>.gh: Tex 8 naming files
// beside it, Mat 21, Mesh 25 without strips, View 7, and its widgets, LabelEx
// 6, ButtonEx 4 and PictureEx 1, each a transform, a style and a text token
// or a picture. A style (ghui/config.dta) names a font, a template Text or a
// Mesh in ghui/resources.rnd. GH2 keeps the same screen as a PanelDir,
// ui/<screen>.milo, of the same objects by the same names, its widgets
// BandLabel 12 and BandButton 10, each of a type that is a font's scene
// (ui/ui_objects.dta), and its script (ui/<screen>.dta) names them.
//
// The scene here is GH2's own object for that screen around GH1's objects:
// the Tex with its file inside, the Mat as it is, the Mesh with strips, the
// View as a Group, each widget as GH2's of the type that is its style's
// font, and each picture as its Mesh. GH1's Cam and Environ stay out: GH2's
// dir names its own. What GH2's script names that GH1's scene has no like
// of comes from GH2's scene, placed where the screen says (gh1/screens.cpp).

#include "gh1/menus.h"

#include "content/setlists.h"
#include "disc/ark.h"
#include "formats/dtb.h"
#include "gh1/look.h"
#include "gh1/scene.h"
#include "gh1/screens.h"
#include "milo/milo.h"

#include <iostream>

namespace gh2::gh1
{
    namespace
    {
        // The buttons a GH1 panel's script links down its lists, each to the
        // next and the last to the first: (navigator (vertical a b c)).
        // Not the last where its navigator has (wrap FALSE).
        void links(const dtb::Node &node, std::map<std::string, std::string> &down, bool wrap = true)
        {
            if (node.type == dtb::kArray && !node.nodes.empty() && node.nodes[0].text == "navigator")
            {
                const dtb::Node *wraps = dtb::find(node, "wrap");
                wrap = !wraps || wraps->nodes.size() < 2u || wraps->nodes[1].integer != 0;
            }
            if (node.type == dtb::kArray && node.nodes.size() > 2u && node.nodes[0].text == "vertical")
                for (size_t i = 1u; i < node.nodes.size(); ++i)
                    if (i + 1u < node.nodes.size() || wrap)
                        down[node.nodes[i].text] = node.nodes[i + 1u < node.nodes.size() ? i + 1u : 1u].text;
            for (const dtb::Node &n : node.nodes)
                links(n, down, wrap);
        }

        // Every symbol and string under `node` that names an object, as
        // scripts do: with an extension.
        void objects(const dtb::Node &node, std::set<std::string> &out)
        {
            if ((node.type == dtb::kSymbol || node.type == dtb::kString) && node.text.find('.') != std::string::npos &&
                node.text.find(' ') == std::string::npos)
                out.insert(node.text);
            for (const dtb::Node &n : node.nodes)
                objects(n, out);
        }

        // That screen's scene in that layer, in place of GH2's. False if it
        // cannot be built.
        bool addScreen(size_t layer, size_t disc, const Screen &screen, const Look &look,
                       const std::map<std::string, std::string> &down, const std::set<std::string> &scripted,
                       std::map<std::string, std::string> &text)
        {
            const std::string name = screen.ours(), theirName = screen.gh1;
            auto gh1 = loadScene(disc, kFolder + theirName + ".gh");
            const auto gh2 = loadScene(0u, "ui/gen/" + name + ".milo_ps2");
            // A name that starts with one renamed, as milo::replacePrefix
            // has it in the bodies.
            const auto as = [&](const std::string &n)
            {
                for (const auto &[from, to] : screen.renamed)
                    if (n.rfind(from, 0) == 0)
                        return to + n.substr(std::string(from).size());
                return n;
            };
            std::map<std::string, std::string> below;
            for (const auto &[from, to] : down)
                below[as(from)] = as(to);
            if (gh1)
            {
                for (auto &entry : gh1->entries)
                    entry.second = as(entry.second);
                for (const auto &[from, to] : screen.renamed)
                    milo::replacePrefix(*gh1, from, to);
            }
            // A scene draws from the View of its name, or one of several
            // (status_1.gh to status_7.gh) from the View of their name.
            const size_t numbered = theirName.find_last_not_of("0123456789");
            const std::string top =
                (numbered != std::string::npos && theirName[numbered] == '_' ? theirName.substr(0, numbered) : theirName) +
                ".view";
            auto made = gh1 && gh2 ? convert(*gh1, *gh2, screen, disc, top, look, below, scripted, text) : std::nullopt;
            if (!made)
            {
                std::cerr << "[gh1] cannot build " << name << "'s menu" << std::endl;
                return false;
            }
            ark::addFile(layer, "ui/gen/" + name + ".milo_ps2", milo::write(*made));
            // Quickplay shows GH1's setlist in GH1's scene from any game's
            // menus, as it does the other games' (content/setlists.h).
            if (name == "sel_song_quickplay")
                setlists::addLook("gh1", *made);
            return true;
        }
    }

    std::set<std::string> addMenus(size_t layer, size_t disc, std::map<std::string, std::string> &text)
    {
        const dtb::Files theirs = [disc](const std::string &path) { return ark::readFile(disc, path); };
        dtb::Macros macros;
        const auto look = lookOf(disc, macros);
        if (!look)
        {
            std::cerr << "[gh1] cannot read GH1's menus" << std::endl;
            return {};
        }
        std::map<std::string, std::string> down;
        if (const auto scripts = dtb::read(std::string(kFolder) + "ui.dta", macros, theirs))
            links(*scripts, down);
        std::set<std::string> scripted;
        dtb::Macros gameMacros;
        if (const auto scripts =
                dtb::read("ui/ui.dta", gameMacros, [](const std::string &path) { return ark::readFile(0u, path); }))
            objects(*scripts, scripted);
        std::set<std::string> built;
        for (const Screen &screen : screens())
            if (addScreen(layer, disc, screen, *look, down, scripted, text))
                built.insert(screen.ours());
        return built;
    }
}
