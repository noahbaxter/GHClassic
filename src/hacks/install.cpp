// A hack of GH2's form: GH2's code over its own files at GH2's paths, as
// Rocks the 80s is (gh80s/install.cpp), every name on it under the hack's
// id (content/sources.h), so its songs and outfits sit beside the retail
// discs' of the same name.

#include "hacks/install.h"

#include "content/campaigns.h"
#include "content/games.h"
#include "content/hidden_guitars.h"
#include "content/setlists.h"
#include "content/sources.h"
#include "disc/ark.h"
#include "formats/dtb.h"

#include <functional>
#include <map>

namespace gh2
{
    namespace
    {
        // Each script on the disc as its own executable reads it: GH2's
        // defines _SHIP, which would take another build's branches.
        void asBuilt(size_t disc, const std::string &build)
        {
            for (const std::string &path : ark::paths(disc, ""))
                if (path.ends_with(".dtb"))
                    ark::addFix(disc, path, [build](std::vector<uint8_t> &bytes)
                                {
                                    auto root = dtb::raw(bytes);
                                    if (root && dtb::resolve(*root, games::builds(), {build}))
                                        bytes = dtb::write(*root);
                                });
        }

        // Its text in one locale.dtb: the root config can list several
        // (Deluxe's 21), which Locale::Init reads from the config GH2's
        // campaign has, listing GH2's one. In front of the renames
        // sources::addDisc makes in it, so they reach every file's.
        void oneLocale(size_t disc)
        {
            const std::string kLocale = "ui/eng/gen/locale.dtb";
            ark::addFix(disc, kLocale, [disc, kLocale](std::vector<uint8_t> &bytes)
                        {
                            const auto config = ark::readFile(disc, "config/gen/gh2.dtb");
                            const auto root = config ? dtb::raw(*config) : std::nullopt;
                            const dtb::Node *list = root ? dtb::find(*root, "locale") : nullptr;
                            if (!list || list->nodes.size() <= 2u)
                                return;
                            dtb::Node merged = dtb::array({});
                            for (size_t i = 1u; i < list->nodes.size(); ++i)
                            {
                                // "../ui/eng/locale_career.dta", from config/.
                                std::string path = list->nodes[i].text;
                                if (path.compare(0u, 3u, "../") == 0)
                                    path.erase(0u, 3u);
                                const size_t slash = path.rfind('/');
                                const size_t dot = path.rfind('.');
                                if (slash == std::string::npos || dot == std::string::npos || dot < slash)
                                    continue;
                                path = path.substr(0u, slash + 1u) + "gen/" + path.substr(slash + 1u, dot - slash - 1u) + ".dtb";
                                const auto file = path == kLocale ? std::optional(bytes) : ark::readFile(disc, path);
                                if (auto part = file ? dtb::raw(*file) : std::nullopt)
                                    merged.nodes.insert(merged.nodes.end(), part->nodes.begin(), part->nodes.end());
                            }
                            bytes = dtb::write(merged);
                        });
        }

        // Each panel's scene by its name, as the UI's scripts on that disc
        // make it, {new Class name (file x.milo) ...}: the archive's path.
        std::map<std::string, std::string> panelScenes(size_t disc)
        {
            std::map<std::string, std::string> scenes;
            const std::function<void(const dtb::Node &)> walk = [&](const dtb::Node &node) {
                const auto &n = node.nodes;
                if (node.type == dtb::kCommand && n.size() >= 4u && n[0].text == "new" && n[2].type == dtb::kSymbol)
                    for (size_t i = 3u; i < n.size(); ++i)
                        if (n[i].type == dtb::kArray && n[i].nodes.size() > 1u && n[i].nodes[0].text == "file")
                        {
                            // From ui/: "sel_venue_mega.milo" is
                            // ui/gen/sel_venue_mega.milo_ps2.
                            const std::string &name = n[i].nodes[1].text;
                            if (name.find('/') == std::string::npos && name.ends_with(".milo"))
                                scenes[n[2].text] = "ui/gen/" + name + "_ps2";
                            break;
                        }
                for (const dtb::Node &child : n)
                    walk(child);
            };
            for (const std::string &path : ark::paths(disc, "ui/gen/"))
                if (path.ends_with(".dtb"))
                    if (const auto file = ark::readFile(disc, path))
                        if (const auto root = dtb::raw(*file))
                            walk(*root);
            return scenes;
        }

        // A panel's scene where it is another file than GH2's: the switch
        // gives a panel the campaign's handlers (content/campaigns.h), and
        // it loads the scene of GH2's name, so that is the campaign's own.
        void ownScenes(size_t disc, size_t layer)
        {
            const std::map<std::string, std::string> gh2 = panelScenes(0u);
            for (const auto &[panel, scene] : panelScenes(disc))
                if (const auto at = gh2.find(panel); at != gh2.end() && at->second != scene)
                    ark::lend(layer, at->second, disc, scene);
        }
    }

    namespace
    {
        // GH2's UI classes' types gain each hack's that GH2 has none of
        // (Deluxe's starsfc label): the UI makes a class's resources once,
        // at boot, from its types (UIManager::InitResources, 0x2152b8), and
        // a label of a type with none has none to draw with. Each loads its
        // scene when a label first uses it, in that hack's campaign.
        void addTypes(const std::vector<size_t> &discs)
        {
            const std::string kObjects = "ui/gen/ui_objects.dtb";
            if (discs.empty())
                return;
            ark::addFix(0u, kObjects, [discs, kObjects](std::vector<uint8_t> &bytes)
                        {
                            auto ours = dtb::raw(bytes);
                            if (!ours)
                                return;
                            const auto find = [](dtb::Node *array, const std::string &key) -> dtb::Node * {
                                if (!array)
                                    return nullptr;
                                for (dtb::Node &n : array->nodes)
                                    if (n.type == dtb::kArray && !n.nodes.empty() && n.nodes[0].text == key)
                                        return &n;
                                return nullptr;
                            };
                            for (const size_t disc : discs)
                            {
                                const auto file = ark::readFile(disc, kObjects);
                                const auto theirs = file ? dtb::raw(*file) : std::nullopt;
                                for (const dtb::Node &cls : theirs ? theirs->nodes : std::vector<dtb::Node>())
                                {
                                    const dtb::Node *types = cls.type == dtb::kArray && !cls.nodes.empty()
                                                                 ? dtb::find(cls, "types") : nullptr;
                                    dtb::Node *ourTypes = types ? find(find(&*ours, cls.nodes[0].text), "types") : nullptr;
                                    if (!ourTypes)
                                        continue;
                                    for (size_t i = 1u; i < types->nodes.size(); ++i)
                                    {
                                        const dtb::Node &type = types->nodes[i];
                                        if (type.type == dtb::kArray && !type.nodes.empty() &&
                                            !dtb::find(*ourTypes, type.nodes[0].text))
                                            ourTypes->nodes.push_back(type);
                                    }
                                }
                            }
                            bytes = dtb::write(*ours);
                        });
        }
    }

    void readHacks()
    {
        games::nameHacks();
        std::vector<size_t> discs;
        for (const std::string &id : games::hacks())
        {
            const size_t disc = *games::disc(id);
            asBuilt(disc, games::build(id));
            oneLocale(disc);
            discs.push_back(disc);
        }
        addTypes(discs);
    }

    void installHacks()
    {
        for (const std::string &id : games::hacks())
        {
            const size_t disc = *games::disc(id);
            // Before its config is read: its names by our ids.
            sources::addDisc(id, disc);
            const size_t layer = ark::addLayer(disc);
            ownScenes(disc, layer);
            stockStore(id, disc, layer);
            campaigns::add(id, layer);
            setlists::addDisc(id, disc, false);
        }
    }
}
