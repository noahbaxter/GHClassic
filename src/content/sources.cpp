#include "content/sources.h"

#include "disc/ark.h"
#include "formats/dtb.h"

#include <functional>
#include <iostream>
#include <memory>
#include <set>

namespace gh2::sources
{
    namespace
    {
        using Names = std::set<std::string>;

        // Config scripts naming a song only as one: its entry, its tier, its
        // price, its co-op scores.
        constexpr const char *kConfigs[] = {"config/gen/songs.dtb", "config/gen/campaign.dtb",
                                            "config/gen/store.dtb", "config/gen/coop_max_scores.dtb"};
        // Screens naming one: the store's stand-in logos (career), the
        // encore's effects (game), the HUD budget test songs, the tutorials.
        constexpr const char *kScreens[] = {"ui/gen/career.dtb", "ui/gen/game.dtb", "ui/gen/hud_budget.dtb",
                                            "ui/gen/tutorials.dtb"};
        // Song names that are also ordinary words in those screens:
        // {meta_music stop} is no song.
        const Names kWords = {"stop"};

        // Each symbol that is one of `names` as `source` names it.
        void qualify(dtb::Node &node, const std::string &source, const Names &names, const Names &skip)
        {
            if (node.type == dtb::kSymbol && names.count(node.text) != 0u && skip.count(node.text) == 0u)
                node.text = song(source, node.text);
            for (dtb::Node &n : node.nodes)
                qualify(n, source, names, skip);
        }

        // Paths under songs/<name>/ moved to songs/<ours>/, the files named
        // by the song with it.
        void retarget(dtb::Node &node, const std::string &source, const std::string &name)
        {
            const std::string file = "songs/" + name + "/" + name, dir = "songs/" + name + "/";
            const std::string ours = song(source, name);
            if (node.type == dtb::kSymbol || node.type == dtb::kString)
            {
                if (node.text.rfind(file, 0) == 0)
                    node.text = "songs/" + ours + "/" + ours + node.text.substr(file.size());
                else if (node.text.rfind(dir, 0) == 0)
                    node.text = "songs/" + ours + "/" + node.text.substr(dir.size());
            }
            for (dtb::Node &n : node.nodes)
                retarget(n, source, name);
        }

        // That disc's `path`, read, changed by `change` and written back.
        void fix(size_t disc, const std::string &path, std::function<void(dtb::Node &)> change)
        {
            ark::addFix(disc, path, [path, change = std::move(change)](std::vector<uint8_t> &bytes)
                        {
                            auto root = dtb::raw(bytes);
                            if (!root)
                            {
                                std::cerr << "[sources] cannot read " << path << std::endl;
                                return;
                            }
                            change(*root);
                            bytes = dtb::write(*root);
                        });
        }
    }

    void entry(dtb::Node &entry, const std::string &source)
    {
        if (entry.type != dtb::kArray || entry.nodes.empty())
            return;
        const std::string name = entry.nodes[0].text;
        entry.nodes[0].text = song(source, name);
        retarget(entry, source, name);
    }

    void addDisc(const std::string &source, size_t disc, const std::vector<std::string> &unlisted)
    {
        const auto file = ark::readFile(disc, kConfigs[0]);
        const auto songs = file ? dtb::raw(*file) : std::nullopt;
        if (!songs)
        {
            std::cerr << "[" << source << "] cannot read its songs" << std::endl;
            return;
        }
        auto names = std::make_shared<Names>();
        for (const dtb::Node &entry : songs->nodes)
            if (entry.type == dtb::kArray && !entry.nodes.empty() && entry.nodes[0].type == dtb::kSymbol)
                names->insert(entry.nodes[0].text);
        names->insert(unlisted.begin(), unlisted.end());

        fix(disc, kConfigs[0], [source, names](dtb::Node &root)
            {
                for (dtb::Node &e : root.nodes)
                    if (e.type == dtb::kArray && !e.nodes.empty() && names->count(e.nodes[0].text) != 0u)
                        entry(e, source);
            });
        for (size_t i = 1u; i < std::size(kConfigs); ++i)
            fix(disc, kConfigs[i], [source, names](dtb::Node &root) { qualify(root, source, *names, {}); });

        std::vector<std::string> screens(std::begin(kScreens), std::end(kScreens));
        for (const std::string &path : ark::paths(disc, "ui/gen/tutorial_"))
            screens.push_back(path);
        for (const std::string &path : screens)
            fix(disc, path, [source, names](dtb::Node &root) { qualify(root, source, *names, kWords); });

        // A song's title is the locale token of its name.
        fix(disc, "ui/eng/gen/locale.dtb", [source, names](dtb::Node &root)
            {
                for (dtb::Node &entry : root.nodes)
                    if (entry.type == dtb::kArray && !entry.nodes.empty() && entry.nodes[0].type == dtb::kSymbol &&
                        names->count(entry.nodes[0].text) != 0u)
                        entry.nodes[0].text = song(source, entry.nodes[0].text);
            });

        // The specific before the folder: the first rename that matches wins.
        for (const std::string &name : *names)
        {
            const std::string ours = song(source, name);
            ark::rename("songs/" + ours + "/" + ours, disc, "songs/" + name + "/" + name);
            ark::rename("songs/" + ours + "/", disc, "songs/" + name + "/");
        }
    }
}
