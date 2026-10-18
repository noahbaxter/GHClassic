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
#include "gh1/guitarist.h"
#include "gh1/look.h"
#include "gh1/scene.h"
#include "gh1/screens.h"
#include "milo/milo.h"

#include <algorithm>
#include <cstring>
#include <iostream>

namespace gh2::gh1
{
    namespace
    {
        using milo::str;

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

        // GH1's store shows a poster for the category picked: the picture's
        // material again for each, over that category's image, for the
        // script to put on it (menus.dta).
        void addPosters(milo::Dir &store, size_t disc)
        {
            const auto body = [&](const std::string &name)
            {
                const auto i = milo::find(store, name);
                return i ? store.bodies[*i] : milo::Bytes();
            };
            const auto mats = named(body("st_poster.pic"), ".mat");
            const milo::Bytes mat = mats.empty() ? milo::Bytes() : body(mats[0]);
            const auto texs = named(mat, ".tex");
            const milo::Bytes tex = texs.empty() ? milo::Bytes() : body(texs[0]);
            for (const char *category : {"guitar", "skin", "song", "character", "video"})
            {
                const auto bitmap =
                    ark::readFile(disc, std::string(kFolder) + "image/gen/shop_" + category + "_poster.png_ps2");
                milo::Bytes image = bitmap ? texWith(tex, *bitmap, true) : milo::Bytes();
                if (image.empty())
                    continue;
                const std::string name = std::string("st_poster_") + category;
                milo::Dir over;
                over.bodies = {mat};
                milo::replacePrefix(over, texs[0], name + ".tex");
                milo::add(store, "Tex", name + ".tex", std::move(image));
                milo::add(store, "Mat", name + ".mat", std::move(over.bodies[0]));
            }
        }

        // GH1's song list writes its rows in its hand: song_element.view's
        // Texts (ghui/resources.rnd), in hand.font or its like on the same
        // sheet (song_header.font, song_blurb.font); its song_stars.txt it
        // never fills (SongProvider::GetTextSlot, GH1 0x11a7d8), and it sits
        // on the score. GH2's stars stay as GH2 has them, between the two,
        // so a song's stars show as in every other game's list. GH2's rows
        // (ui_objects.dta's UIList song, list_song.milo) are in
        // dyingmarker, whose small letters are capitals and whose & is a +.
        // Here GH2's list takes GH1's Texts by what each shows: their font,
        // scale, place, alignment, wrap, leading and size, the element
        // placed where GH2's puts its song's name.
        // It is a type of its own, which GH1's lists take (addScreen), as
        // quickplay shows GH1's setlist under any campaign.
        constexpr const char *kSongList = "song_gh1";

        // False if it cannot be made, for GH1's lists to stay GH2's.
        bool addSongList(const Look &look)
        {
            constexpr const char *kObjects = "ui/gen/ui_objects.dtb";
            const auto objects = ark::readFile(0u, kObjects);
            auto root = objects ? dtb::raw(*objects) : std::nullopt;
            dtb::Node *types = root ? const_cast<dtb::Node *>(dtb::find(*root, "UIList")) : nullptr;
            types = types ? const_cast<dtb::Node *>(dtb::find(*types, "types")) : nullptr;
            const dtb::Node *song = types ? dtb::find(*types, "song") : nullptr;
            auto list = loadScene(0u, "ui/gen/list_song.milo_ps2");
            if (!song || !list)
            {
                std::cerr << "[gh1] cannot read GH2's song list" << std::endl;
                return false;
            }
            dtb::Node type = *song;
            type.nodes[0].text = kSongList;
            const std::string file = std::string("list_") + kSongList;
            if (dtb::Node *resource = const_cast<dtb::Node *>(dtb::find(type, "resource_file"));
                resource && resource->nodes.size() > 1u)
                resource->nodes[1].text = file + ".milo";
            types->nodes.push_back(std::move(type));

            // A GH2 Text 17 has Hmx::Object's header, then a Draw 3 (a
            // revision, whether it shows, a sphere and an order), then its
            // Trans 9; a Mesh has its Trans first, then its Draw. A Trans
            // has its local, a 3x3 and a position, then its world.
            const auto own = [](const milo::Bytes &b) { return headerEnd(b, 4u).value_or(0u); };
            const auto theirs = [&](const char *name) -> const milo::Bytes *
            {
                const auto i = milo::find(look.resources, name);
                return i ? &look.resources.bodies[*i] : nullptr;
            };
            const auto ours = [&](const char *name) -> milo::Bytes *
            {
                const auto i = milo::find(*list, name);
                return i ? &list->bodies[*i] : nullptr;
            };
            const milo::Bytes *name = theirs("song_name.txt");
            const milo::Bytes *row = ours("list.txt");
            const auto named = name ? parts("Text", *name) : std::nullopt;
            if (!named || !row)
                return false;
            // The dir's subdirs, a count and their paths: GH1's font's ahead
            // of dyingmarker's, which GH2's stars keep.
            size_t at = named->rest;
            milo::Bytes one, two;
            milo::putU32(one, 1u);
            milo::putStr(one, "../dyingmarker.milo");
            milo::putU32(two, 2u);
            milo::putStr(two, "../" + typeOf(str(*name, at)) + ".milo");
            milo::putStr(two, "../dyingmarker.milo");
            const auto sub = std::search(list->root.begin(), list->root.end(), one.begin(), one.end());
            if (sub == list->root.end())
                return false;
            list->root.insert(list->root.erase(sub, sub + static_cast<std::ptrdiff_t>(one.size())), two.begin(), two.end());
            const float down = f32(*row, own(*row) + 25u + 48u) - f32(*name, named->trans.at + 48u);
            const std::pair<const char *, const char *> kTexts[] = {
                {"list.txt", "song_name.txt"},
                {"header.txt", "song_header.txt"},
                {"score.txt", "song_score.txt"},
                {"blurb.txt", "song_blurb.txt"},
            };
            for (const auto &[gh2, gh1] : kTexts)
            {
                milo::Bytes *b = ours(gh2);
                const milo::Bytes *of = theirs(gh1);
                const auto p = of ? parts("Text", *of) : std::nullopt;
                if (!b || !p)
                    continue;
                const size_t trans = own(*b) + 25u, font = transEnd(*b, trans);
                for (const size_t local : {trans + 4u, trans + 52u})
                {
                    std::memcpy(b->data() + local, of->data() + p->trans.at + 4u, 48u);
                    const float z = f32(*b, local + 44u) + down;
                    std::memcpy(b->data() + local + 44u, &z, 4u);
                }
                // Then its font, alignment and text, then a colour, wrap,
                // leading, fixed length, italics and size, as a GH1 Text 15
                // has them. The text is the list's to set. Last are whether
                // it takes markup and its caps mode (RndText::Load, debug
                // 0x207830): GH2's are in capitals, GH1's in their own case.
                size_t o = font, t = p->rest;
                str(*b, o);
                const size_t text = o + 4u;
                o = text;
                str(*b, o);
                milo::Bytes out(b->begin(), b->begin() + static_cast<std::ptrdiff_t>(font));
                milo::putStr(out, typeOf(str(*of, t)) + ".font");
                put(out, *of, t, t + 4u);
                put(out, *b, text, o);
                t += 4u;
                str(*of, t);
                put(out, *of, t, t + 36u);
                put(out, *b, o + 36u, o + 37u);
                milo::putU32(out, 0u);
                *b = std::move(out);
            }
            ark::addFile(kObjects, dtb::write(*root));
            ark::addFile("ui/gen/" + file + ".milo_ps2", milo::write(*list));
            return true;
        }

        // A GH2 widget as another type of its class: its class's and
        // UIComponent's revisions, Hmx::Object's, then the type.
        void retype(milo::Dir &dir, const std::string &name, const std::string &type)
        {
            const auto i = milo::find(dir, name);
            if (!i)
                return;
            milo::Bytes &b = dir.bodies[*i];
            size_t o = 12u;
            str(b, o);
            milo::Bytes out(b.begin(), b.begin() + 12);
            milo::putStr(out, type);
            put(out, b, o, b.size());
            b = std::move(out);
        }

        // That screen's scene in that layer, in place of GH2's, its song list
        // GH1's with `songList`. False if it cannot be built.
        bool addScreen(size_t layer, size_t disc, const Screen &screen, const Look &look,
                       const std::map<std::string, std::string> &down, const std::set<std::string> &scripted,
                       bool songList, std::map<std::string, std::string> &text)
        {
            const std::string name = screen.ours(), theirName = screen.gh1;
            auto gh1 = loadScene(disc, kFolder + theirName + ".gh");
            const auto gh2 = loadScene(0u, "ui/gen/" + name + ".milo_ps2");
            if (const auto also = gh1 && screen.also ? loadScene(disc, kFolder + std::string(screen.also) + ".gh")
                                                     : std::nullopt)
            {
                gh1->entries.insert(gh1->entries.end(), also->entries.begin(), also->entries.end());
                gh1->bodies.insert(gh1->bodies.end(), also->bodies.begin(), also->bodies.end());
            }
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
            if (name == "store")
                addPosters(*made, disc);
            if (songList && theirName == "sel_song")
                retype(*made, "ss_song.lst", kSongList);
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
        // GH1's names for a band's status, which the game finds by number
        // (status_%d), the lines its newspapers take a name into, and the
        // line its win screen takes the difficulty into.
        for (const auto &[token, string] : look->strings)
            if (token.rfind("status_", 0) == 0 || token == "win_diff")
                text[token] = string;
        // Each character's photo for those newspapers, by GH2's name for
        // the character, for the script to load (menus.dta).
        for (const Guitarist &g : kGuitarists)
            if (auto photo = ark::readFile(disc, std::string(kFolder) + "image/gen/status_" + g.folder + "0.png_ps2"))
                ark::addFile(layer, std::string("ui/image/gen/gh1_status_") + g.character + ".png_ps2", std::move(*photo));
        // GH1's credits are in its serif face, their headings in its
        // impactor in orange (resources.rnd's credit_title.txt and
        // credits_center.mat): GH2's list for credits with those of its own
        // fonts' scenes in place of clarendon and rockletters. The orange
        // is not carried: the list colours its text by its type
        // (ui_objects.dta's UIList credits).
        if (auto list = loadScene(0u, "ui/gen/list_credits.milo_ps2"))
        {
            milo::replacePrefix(*list, "../clarendon.milo", "../serif.milo");
            milo::replacePrefix(*list, "../rockletters.milo", "../impactor.milo");
            milo::replacePrefix(*list, "clarendon.font", "serif.font");
            milo::replacePrefix(*list, "rockletters.font", "impactor.font");
            ark::addFile(layer, "ui/gen/list_credits.milo_ps2", milo::write(*list));
        }
        const bool songList = addSongList(*look);
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
            if (addScreen(layer, disc, screen, *look, down, scripted, songList, text))
                built.insert(screen.ours());
        return built;
    }
}
