#include "content/card.h"

#include "disc/ark.h"
#include "formats/dtb.h"
#include "save/ps2_card.h"
#include "save/save.h"

#include <iostream>

namespace gh2::content
{
    namespace
    {
        // `key`'s value in a (key value...) array.
        const dtb::Node *value(const dtb::Node &array, const std::string &key, size_t at = 1)
        {
            const dtb::Node *found = dtb::find(array, key);
            return found && found->nodes.size() > at ? &found->nodes[at] : nullptr;
        }

        template <typename T, size_t N>
        void numbers(const dtb::Node &array, std::array<T, N> &out)
        {
            for (size_t i = 0; i < N && i < array.nodes.size(); ++i)
                out[i] = static_cast<T>(dtb::number(array.nodes[i]).value_or(0.0f));
        }

        // (key (a b c) (a b c)...) into each row.
        template <typename T, size_t N, size_t M>
        void rows(const dtb::Node &icon, const std::string &key, std::array<std::array<T, N>, M> &out)
        {
            const dtb::Node *found = dtb::find(icon, key);
            for (size_t i = 0; found && i < M && i + 1 < found->nodes.size(); ++i)
                numbers(found->nodes[i + 1], out[i]);
        }
    }

    void addCardGame(size_t disc, const std::string &game, bool gh1Layout, uint32_t dataSize, const std::string &title,
                     int lineBreak)
    {
        const dtb::Files files = [disc](const std::string &path) { return ark::readFile(disc, path); };
        dtb::Macros macros;
        const auto mc = dtb::read("config/mc.dta", macros, files);
        const dtb::Node *dir = mc ? value(*mc, "base_dir") : nullptr;
        const dtb::Node *icon = mc ? dtb::find(*mc, "ps2_icon") : nullptr;
        const dtb::Node *file = icon ? value(*icon, "file") : nullptr;
        const auto iconBytes = file ? ark::readFile(disc, "mc/" + file->text) : std::nullopt;
        if (!dir || !icon || !iconBytes)
        {
            std::cerr << "[" << game << "] cannot read its memory card setup" << std::endl;
            return;
        }
        save::IconSys sys;
        if (const dtb::Node *transparency = value(*icon, "bg_transparency"))
            sys.transparency = static_cast<uint32_t>(dtb::number(*transparency).value_or(0.0f));
        rows(*icon, "bg_colors", sys.colors);
        rows(*icon, "lit_dirs", sys.lightDirs);
        rows(*icon, "lit_colors", sys.lightColors);
        if (const dtb::Node *ambient = dtb::find(*icon, "ambient"))
            for (size_t i = 0; i < 3 && i + 1 < ambient->nodes.size(); ++i)
                sys.ambient[i] = dtb::number(ambient->nodes[i + 1]).value_or(0.0f);
        sys.title = title;
        sys.lineBreak = lineBreak;
        sys.iconFile = file->text;
        save::addCardGame({game, dir->text, gh1Layout, dataSize, save::makeIconSys(sys), file->text,
                           std::vector<uint8_t>(iconBytes->begin(), iconBytes->end())});
    }
}
