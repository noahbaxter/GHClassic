#include "content/games.h"

#include "disc/ark.h"
#include "disc/sha1.h"
#include "disc/volume.h"
#include "formats/dtb.h"
#include "script.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <iostream>
#include <map>
#include <memory>

namespace gh2::games
{
    namespace
    {
        struct Release
        {
            const char *game;
            const char *serial;     // ark::discWithSerial's
            const char *headerSha1; // GEN/MAIN.HDR's, "" where not known
        };

        // Every supported release, from config/executables.toml (CMake).
        constexpr Release kReleases[] = {
#include "releases.inc"
        };

        // tools/ghc.py's content_games gives a dev run the same order.
        constexpr const char *kOrder[] = {"gh80s", "gh1", "gh2x"};
        constexpr const char *kEngine = "gh2";

        struct Hack
        {
            size_t added;       // its addDisc, from 0
            std::string build;  // what its executable defines in _SHIP's place
            std::string header; // GEN/MAIN.HDR's SHA-1
            std::string name;   // its image's file name, what the menus call it
            std::string id;     // named once its archive is indexed
            bool career = false; // its own career screens (ui/career.dta)
        };
        std::vector<Hack> s_hacks;

        std::string headerOf(Volume &volume)
        {
            const auto header = volume.find("GEN/MAIN.HDR");
            if (!header)
                return "";
            std::vector<uint8_t> bytes(static_cast<size_t>(header->size));
            if (volume.read(header->offset, bytes.data(), bytes.size()) != bytes.size())
                return "";
            return sha1Hex(bytes.data(), bytes.size());
        }

        // What the boot executable defines where GH2's defines _SHIP: the
        // name after HX_EE in the table of names it defines.
        std::string buildOf(Volume &volume)
        {
            const auto elf = volume.find(volume.serial());
            std::vector<uint8_t> bytes(elf ? static_cast<size_t>(elf->size) : 0u);
            if (elf && volume.read(elf->offset, bytes.data(), bytes.size()) == bytes.size())
            {
                static constexpr char kBefore[] = "HX_EE";
                const auto at = std::search(bytes.begin(), bytes.end(), kBefore, kBefore + sizeof kBefore);
                auto name = at == bytes.end() ? at : at + sizeof kBefore;
                while (name != bytes.end() && *name == 0u)
                    ++name;
                const auto end = std::find(name, bytes.end(), uint8_t{0});
                const std::string build(name, end);
                if (!build.empty() && build.size() < 16u)
                    return build;
            }
            return "_SHIP";
        }

        // "BAGHCE-68878" -> "ghce68878": a hack's save folder, its region
        // (BA, BE, BI) dropped, which its author keeps its own so its saves
        // never meet another game's on a card.
        std::string idOf(const std::string &folder)
        {
            std::string id;
            for (size_t i = folder.size() > 2u ? 2u : 0u; i < folder.size(); ++i)
                if (std::isalnum(static_cast<unsigned char>(folder[i])))
                    id += static_cast<char>(std::tolower(static_cast<unsigned char>(folder[i])));
            return id;
        }

        // Its config's save folder as its own executable reads it.
        std::string folderOf(size_t disc, const std::string &build)
        {
            const auto file = ark::readFile(disc, "config/gen/mc.dtb");
            auto root = file ? dtb::raw(*file) : std::nullopt;
            if (!root)
                return "";
            dtb::resolve(*root, builds(), {build});
            const dtb::Node *dir = dtb::find(*root, "base_dir");
            return dir && dir->nodes.size() > 1u ? dir->nodes[1].text : "";
        }

        bool taken(const std::string &id)
        {
            for (const Release &release : kReleases)
                if (id == release.game || id == idOf(std::string("BA") + release.serial))
                    return true;
            return std::any_of(s_hacks.begin(), s_hacks.end(), [&](const Hack &hack) { return hack.id == id; });
        }

        script::Node mountedCommand(const script::Call &call)
        {
            return {mounted(call.symbol(1)) ? 1u : 0u, script::kInt};
        }

        script::Node hacksCommand(const script::Call &call)
        {
            const bool careers = call.size() > 1 && call.symbol(1) == "career";
            std::string ids;
            for (const std::string &id : hacks())
                if (!careers || hasCareer(id))
                    ids += id + " ";
            return {script::parse(call.rdram, call.ctx, call.runtime, ids), script::kArray};
        }
    }

    const std::set<std::string> &builds()
    {
        static const std::set<std::string> kBuilds = {"_SHIP", "_ENCO", "_MEGA", "_ONYX", "_DRUM",
                                                      "2DBUG", "8DBUG", "_2DBUG", "GH1RX"};
        return kBuilds;
    }

    void mount(const std::vector<std::filesystem::path> &images)
    {
        std::map<std::string, std::filesystem::path> found;
        std::vector<std::pair<std::filesystem::path, Hack>> hacks;
        for (const std::filesystem::path &image : images)
        {
            std::string error;
            const std::unique_ptr<Volume> volume = Volume::open(image, error);
            if (!volume)
            {
                std::cerr << "[games] " << image.filename().string() << ": " << error << std::endl;
                continue;
            }
            const std::string serial = volume->serial();
            const Release *release = nullptr;
            for (const Release &r : kReleases)
                if (serial == r.serial)
                    release = &r;
            // A release's own header, or one whose header is not known.
            const std::string header = headerOf(*volume);
            const bool retail = release && (!*release->headerSha1 || header == release->headerSha1);
            if (retail)
            {
                if (std::string(release->game) != kEngine)
                    found.emplace(release->game, image);
            }
            else if (header.empty())
                std::cerr << "[games] " << image.filename().string() << ": " << serial
                          << " is no release this build takes content from" << std::endl;
            else if (release && std::string(release->game) == "gh1")
                std::cerr << "[games] " << image.filename().string() << ": a hack of GH1, whose engine takes none yet"
                          << std::endl;
            else
                hacks.push_back({image, {0u, buildOf(*volume), header, image.stem().string()}});
        }
        std::vector<std::string> order(std::begin(kOrder), std::end(kOrder));
        for (const auto &[game, image] : found)
            if (std::find(order.begin(), order.end(), game) == order.end())
                order.push_back(game);
        size_t added = 0u;
        for (const std::string &game : order)
            if (const auto it = found.find(game); it != found.end() && ark::addDisc(it->second))
                ++added;
        for (auto &[image, hack] : hacks)
            if (ark::addDisc(image))
            {
                hack.added = added++;
                s_hacks.push_back(hack);
            }
    }

    void nameHacks()
    {
        for (Hack &hack : s_hacks)
        {
            const auto disc = ark::discAdded(hack.added);
            if (!disc)
                continue;
            // A slot swap keeps its retail folder: its header names it.
            std::string id = idOf(folderOf(*disc, hack.build));
            if (id.empty() || taken(id))
                id = "h" + hack.header.substr(0u, 8u);
            if (taken(id))
                std::cerr << "[games] " << id << " is mounted twice; the second left out" << std::endl;
            else
                hack.id = id;
            hack.career = !ark::paths(*disc, "ui/gen/career.dtb").empty();
            std::cerr << "[games] hack " << hack.id << " (" << hack.build << (hack.career ? ", career" : "") << "): "
                      << hack.name << std::endl;
        }
    }

    std::optional<size_t> disc(const std::string &game)
    {
        for (const Hack &hack : s_hacks)
            if (!hack.id.empty() && game == hack.id)
                return ark::discAdded(hack.added);
        for (const Release &release : kReleases)
            if (game == release.game)
                if (const auto found = ark::discWithSerial(release.serial))
                    return found;
        return std::nullopt;
    }

    bool mounted(const std::string &game) { return disc(game).has_value(); }

    std::vector<std::string> hacks()
    {
        std::vector<std::string> ids;
        for (const Hack &hack : s_hacks)
            if (!hack.id.empty() && ark::discAdded(hack.added))
                ids.push_back(hack.id);
        return ids;
    }

    bool isHack(size_t disc)
    {
        return std::any_of(s_hacks.begin(), s_hacks.end(),
                           [disc](const Hack &hack) { return ark::discAdded(hack.added) == disc; });
    }

    bool hasCareer(const std::string &hack)
    {
        return std::any_of(s_hacks.begin(), s_hacks.end(),
                           [&](const Hack &known) { return known.id == hack && known.career; });
    }

    std::string name(const std::string &hack)
    {
        for (const Hack &known : s_hacks)
            if (hack == known.id)
                return known.name;
        return hack;
    }

    std::string build(const std::string &hack)
    {
        for (const Hack &known : s_hacks)
            if (hack == known.id)
                return known.build;
        return "_SHIP";
    }

    void install(PS2Runtime &, const Addresses &)
    {
        script::addCommand("mounted", mountedCommand);
        script::addCommand("hacks", hacksCommand);
    }
}
