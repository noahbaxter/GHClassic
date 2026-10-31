#include "content/setlist_look.h"

#include "content/setlist_paper.h"
#include "content/setlist_printout.h"
#include "disc/ark.h"
#include "formats/dtb.h"
#include "gh1/scene.h"
#include "ui/text_fit.h"

#include <cstring>
#include <functional>
#include <iostream>
#include <vector>

namespace gh2::setlist_look
{
    namespace
    {
        // A two-line setlist's rows, in the row's own units (about 2.26
        // pixels at 1080p, rows 40 apart down from 0): the song's name and
        // the line under it, each its left end, its foot and its size, the
        // name three quarters of GH2's 26 and the line under it smaller and
        // in from it. Each is cut before kRowsEnd, where the custom list's
        // printout starts (ui/dta/quickplay.dta), less a margin.
        struct Line
        {
            float x, foot, size;
        };
        constexpr Line kTitle = {-263.0f, -30.7f, 19.5f};
        constexpr Line kUnder = {-252.0f, -41.9f, 11.5f};
        // The tier's header over a row, as GH2's list_song2.milo has it,
        // cut there too.
        constexpr Line kHeader = {-294.0f, 1.0f, 25.0f};
        constexpr float kRowsEnd = 50.0f;
        constexpr uint32_t kBottomLeft = 0x41u; // RndText's alignment: its foot on the line
        constexpr uint32_t kForceUpper = 2u;    // RndText's caps mode: in capitals

        // A GH2 Text 17 (RndText::Load, debug 0x207830) put on that line,
        // upright and at its own size (GH2's blurb is at 0.7 and turned a
        // little), in capitals: Hmx::Object's header, a Draw 3 (a revision, whether it shows, a
        // sphere and an order), its Trans 9 (a revision, then its local
        // and world, each a 3x3 and a position), then its font, alignment
        // and text, a colour, wrap, leading, fixed length, italics and
        // size, whether it takes markup and its caps mode.
        bool reshape(milo::Bytes &b, const Line &line)
        {
            const auto header = gh1::headerEnd(b, 4u);
            if (!header || *header + 25u + 100u > b.size())
                return false;
            const size_t trans = *header + 25u;
            size_t o = gh1::transEnd(b, trans);
            milo::str(b, o);
            const size_t alignment = o;
            o += 4u;
            milo::str(b, o);
            if (o + 41u > b.size())
                return false;
            constexpr float kUpright[9] = {1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f};
            for (const size_t local : {trans + 4u, trans + 52u})
            {
                std::memcpy(b.data() + local, kUpright, sizeof kUpright);
                std::memcpy(b.data() + local + 36u, &line.x, 4u);
                std::memcpy(b.data() + local + 44u, &line.foot, 4u);
            }
            std::memcpy(b.data() + alignment, &kBottomLeft, 4u);
            std::memcpy(b.data() + o + 32u, &line.size, 4u);
            std::memcpy(b.data() + o + 37u, &kForceUpper, 4u);
            return true;
        }

        // A milo path's folder, with its slash.
        std::string folderOf(const std::string &path)
        {
            return path.substr(0u, path.rfind('/') + 1u);
        }
    }

    // It names the menus' camera beside it (../metacam.milo), which under
    // <game>/ is another camera than the one the screens turn
    // (GHScreen::AnimateTransition, 0x145e18). From the ARK root it is the
    // same one.
    void addScene(const std::string &path, milo::Dir scene)
    {
        milo::replacePrefix(scene, "../metacam.milo", "../../../ui/metacam.milo");
        setlist_paper::endlessPaper(scene);
        setlist_printout::add(scene);
        ark::addFile(path, milo::write(scene));
    }

    std::optional<std::string> addTwoLineLook(const std::string &name, const std::string &look)
    {
        constexpr const char *kObjects = "ui/gen/ui_objects.dtb";
        auto scene = gh1::loadScene(0u, kOwnScene);
        const auto lst = scene ? milo::find(*scene, "ss_song.lst") : std::nullopt;
        // As the game gets it: with GH1's list type, where that has
        // been added (gh1/menus.cpp).
        const auto objects = ark::readFile(kObjects);
        auto root = objects ? dtb::raw(*objects) : std::nullopt;
        auto *types = root ? const_cast<dtb::Node *>(dtb::find(*root, "UIList")) : nullptr;
        types = types ? const_cast<dtb::Node *>(dtb::find(*types, "types")) : nullptr;
        if (!lst || !types)
        {
            std::cerr << "[setlist] cannot read GH2's song list, " << name << " in one-line rows" << std::endl;
            return std::nullopt;
        }
        // A UIList's body: its class's and UIComponent's revisions,
        // Hmx::Object's, then its type.
        milo::Bytes &list = scene->bodies[*lst];
        size_t at = 12u;
        const std::string was = milo::str(list, at);
        const dtb::Node *theirs = dtb::find(*types, was);
        dtb::Node type = theirs ? *theirs : dtb::Node{};
        auto *resource = const_cast<dtb::Node *>(dtb::find(type, "resource_file"));
        auto *colors = const_cast<dtb::Node *>(dtb::find(type, "colors"));
        const std::string file = resource && resource->nodes.size() > 1u ? resource->nodes[1].text : "";
        auto rows = file.ends_with(".milo") ? gh1::loadScene(0u, "ui/gen/" + file + "_ps2") : std::nullopt;
        const auto title = rows ? milo::find(*rows, "list.txt") : std::nullopt;
        const auto blurb = rows ? milo::find(*rows, "blurb.txt") : std::nullopt;
        if (!colors || colors->nodes.size() < 2u || colors->nodes[1].nodes.size() < 7u || !title || !blurb ||
            !reshape(rows->bodies[*title], kTitle) || !reshape(rows->bodies[*blurb], kUnder))
        {
            std::cerr << "[setlist] GH2's song list type " << was << " not as GH2 has it, " << name
                      << " in one-line rows" << std::endl;
            return std::nullopt;
        }
        const std::string own = "song_" + name, ownFile = "list_song_" + name + ".milo";
        type.nodes[0].text = own;
        resource->nodes[1].text = ownFile;
        std::vector<dtb::Node> &each = colors->nodes[1].nodes;
        const std::function<void(dtb::Node &)> lighter = [&](dtb::Node &node)
        {
            if (node.type == dtb::kFloat && node.real == 0.1f)
                node.real = 0.4f;
            for (dtb::Node &n : node.nodes)
                lighter(n);
        };
        each[6] = each[1];
        lighter(each[6]);
        types->nodes.push_back(std::move(type));
        milo::Bytes retyped(list.begin(), list.begin() + 12);
        milo::putStr(retyped, own);
        retyped.insert(retyped.end(), list.begin() + static_cast<std::ptrdiff_t>(at), list.end());
        list = std::move(retyped);
        ark::addFile(kObjects, dtb::write(*root));
        ark::addFile("ui/gen/" + ownFile + "_ps2", milo::write(*rows));
        const std::string folder = folderOf(look);
        addScene(folder + "gen/sel_song_" + name + ".milo_ps2", std::move(*scene));
        return folder + "sel_song_" + name + ".milo";
    }

    std::string rowText(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t text, const std::string &line,
                        uint32_t element)
    {
        const Line &on = element == 0u ? kHeader : element == 1u ? kTitle : kUnder;
        return text_fit::fit(rdram, ctx, runtime, text, text_fit::upper(line), on.size, kRowsEnd - on.x);
    }
}
