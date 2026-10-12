// A 360 venue, character or guitar goes in as a PS2 scene file at the
// PS2's path, so the engine's own camera shots, light presets, clips and
// crowd run it (.planning/gh2x-look/DESIGN.md, "Recommendation"). A picture
// stays on the host: the guest holds a stand-in at most 64 pixels a side
// naming it (render/texture_store.h), where the 360's own would be 10 to 60
// times a song's free heap.
//
// A guitar skin draws its outfit file with a Mat of that file by name, as
// the game disc's config/guitars.dta gives it: (lp_cayenne (outfit
// lp_cayenne) (mat lp_cayenne.mat)). The 360 names four of the 59 otherwise
// (lp_cayenne, sg_cherry, sg_white, es_cherry), and its sg_white is a file
// of its own, so a converted guitar also holds the 360 skin's Mat, and the
// pictures it draws, under the name the game disc asks for.

#include "gh2x/models.h"

#include "content/games.h"
#include "disc/ark.h"
#include "formats/dtb.h"
#include "gh2x/scene.h"
#include "render/look.h"
#include "render/texture_store.h"
#include "settings/settings.h"

#include <algorithm>
#include <cstring>
#include <iostream>
#include <map>
#include <memory>

namespace gh2
{
    namespace
    {
        constexpr const char *kVenues = "world/";
        // GH2's band: config's default_band and every song's (band ...).
        constexpr const char *kBand[] = {"metal_singer", "metal_bass", "metal_drummer", "metal_keyboard",
                                         "female_singer"};
        constexpr const char *kFolders[] = {kVenues, "char/"};
        constexpr const char *kGuitars = "char/og/guitars/gen/";
        // The track: the PS2's scene, and the one of the 360's two that it
        // draws (its track.milo_xbox is the PS2's, object for object).
        constexpr const char *kTrack = "track/gen/track.milo_ps2";
        constexpr const char *kTrackTwin = "track/gen/track_new.milo_xbox";
        constexpr const char *kLuminanceMap = "world/battle/ng/textures/gen/arena_luminancetest04.png_xbox";
        constexpr uint32_t kStandInSide = 64u;
        // The smallest a stand-in has room for its payload's name in.
        constexpr uint32_t kStandInLeast = 16u;

        bool holds(const std::vector<std::string> &paths, const std::string &path)
        {
            return std::binary_search(paths.begin(), paths.end(), path);
        }

        // The Mat a game disc skin asks its outfit file for, and the 360
        // skin of the same name's outfit file and Mat.
        struct Skin
        {
            std::string mat;
            std::string xboxOutfit;
            std::string xboxMat;
        };

        struct Discs
        {
            size_t xbox = 0u;
            // Sorted paths: the PS2 disc's, and those with the 360-only
            // scenes served beside them.
            std::vector<std::string> ps2;
            std::vector<std::string> any;
            // By the game disc's outfit file.
            std::map<std::string, std::vector<Skin>> skins;
        };

        // Each skin of a guitars.dta: guitar and skin, outfit and Mat.
        std::map<std::pair<std::string, std::string>, std::pair<std::string, std::string>>
        readSkins(size_t disc)
        {
            std::map<std::pair<std::string, std::string>, std::pair<std::string, std::string>> out;
            dtb::Macros macros;
            const auto guitars = dtb::read("config/guitars.dta", macros,
                                           [disc](const std::string &path) { return ark::readFile(disc, path); });
            const auto value = [](const dtb::Node &array, const char *key)
            {
                const dtb::Node *found = dtb::find(array, key);
                return found && found->nodes.size() > 1u ? found->nodes[1].text : std::string();
            };
            for (const dtb::Node &guitar : guitars ? guitars->nodes : std::vector<dtb::Node>{})
            {
                const dtb::Node *skins = guitar.type == dtb::kArray ? dtb::find(guitar, "skins") : nullptr;
                for (size_t i = 1u; skins && i < skins->nodes.size(); ++i)
                {
                    const dtb::Node &skin = skins->nodes[i];
                    if (skin.type == dtb::kArray && !skin.nodes.empty())
                        out[{guitar.nodes[0].text, skin.nodes[0].text}] = {value(skin, "outfit"), value(skin, "mat")};
                }
            }
            return out;
        }

        std::string stem(const std::string &path)
        {
            const size_t slash = path.rfind('/') + 1u;
            return path.substr(slash, path.rfind('.') - slash);
        }

        // Each Mat's 360 shading, by name.
        using Looks = std::map<std::string, look::Look>;

        std::optional<milo::Dir> convert(const Discs &discs, const std::string &twin, const std::string &path,
                                         Looks &looks)
        {
            const auto theirs = ark::readFile(discs.xbox, twin);
            const auto other = theirs ? milo::read(*theirs) : std::nullopt;
            if (!other || other->version != 25u)
                return std::nullopt;
            // A scene only the 360 has (its characters' viseme and menu
            // clips) stands against an empty one of its own name.
            std::optional<milo::Dir> scene = milo::Dir{24u, other->className, other->name};
            if (holds(discs.ps2, path))
            {
                const auto ours = ark::readFile(0u, path);
                scene = ours ? milo::read(*ours) : std::nullopt;
            }
            if (!scene || scene->version != 24u)
                return std::nullopt;
            const gh2x::SceneSource source{
                *other, *scene, path, [&](const std::string &file) { return holds(discs.any, file); },
                [&](const std::string &tex, uint32_t width, uint32_t height)
                {
                    while (width > kStandInSide || height > kStandInSide)
                    {
                        width = std::max(width / 2u, 1u);
                        height = std::max(height / 2u, 1u);
                    }
                    return textures::standIn(textures::add({discs.xbox, twin, tex}), std::max(width, kStandInLeast),
                                             std::max(height, kStandInLeast));
                },
                [&](const std::string &mat, const gh2x::Mat &shading)
                {
                    // Normal, glow, specular, diffuse ramp and cube, as
                    // matToPs2 lists them.
                    const auto map = [&](size_t i)
                    { return shading.maps[i].empty() ? 0u : textures::add({discs.xbox, twin, shading.maps[i]}); };
                    look::Look &look = looks[mat];
                    look.diffuse = shading.diffuse.empty() ? 0u : textures::add({discs.xbox, twin, shading.diffuse});
                    look.normal = map(0u);
                    look.glow = map(1u);
                    look.specular = map(2u);
                    look.ramp = map(3u);
                    look.cube = map(4u);
                    look.emissive = shading.emissive;
                    std::copy_n(shading.specular, 4, look.specularColor);
                    look.normalize = shading.normalize;
                },
                [](const std::string &system, const gh2x::Spin &spin)
                { look::addSpin(system, spin.mat, spin.rpm[0], spin.rpm[1]); }};
            return path == kTrack ? gh2x::convertTrack(source) : gh2x::convertScene(source);
        }

        bool has(const milo::Dir &dir, const std::string &name) { return milo::find(dir, name).has_value(); }

        // The Mats the game disc's skins ask a converted guitar for that it
        // lacks: the 360 skin's, from this file or the 360's own for it,
        // with each picture it names.
        void addSkins(const Discs &discs, const std::string &path, const std::string &twin, milo::Dir &dir,
                      Looks &looks)
        {
            const auto found = discs.skins.find(stem(path));
            if (!path.starts_with(kGuitars) || found == discs.skins.end())
                return;
            for (const Skin &skin : found->second)
            {
                if (has(dir, skin.mat))
                    continue;
                std::optional<milo::Dir> own;
                Looks ownLooks;
                if (skin.xboxOutfit != stem(twin))
                {
                    const std::string file = "char/ng/guitars/gen/" + skin.xboxOutfit + ".milo_xbox";
                    own = convert(discs, file, gh2x::ps2Path(file), ownLooks);
                }
                const milo::Dir &from = own ? *own : dir;
                const Looks &fromLooks = own ? ownLooks : looks;
                if (const auto shading = fromLooks.find(skin.xboxMat); shading != fromLooks.end())
                    looks[skin.mat] = shading->second;
                const auto mat = milo::find(from, skin.xboxMat);
                if (!mat || from.entries[*mat].first != "Mat")
                {
                    std::cerr << "[gh2x] " << path << ": no " << skin.xboxMat << " for " << skin.mat << std::endl;
                    continue;
                }
                const milo::Bytes body = from.bodies[*mat];
                for (size_t i = 0; own && i < own->entries.size(); ++i)
                {
                    milo::Bytes name;
                    milo::putStr(name, own->entries[i].second);
                    if (own->entries[i].first == "Tex" && !has(dir, own->entries[i].second) &&
                        std::search(body.begin(), body.end(), name.begin(), name.end()) != body.end())
                        milo::add(dir, "Tex", own->entries[i].second, own->bodies[i]);
                }
                milo::add(dir, "Mat", skin.mat, body);
            }
        }

        // Whether the 360's scene is the one served at a PS2 path now.
        bool wanted(const std::string &path)
        {
            // A band member is the band setting's first: the 360's or the
            // PS2's whatever the graphics, when it names either.
            for (const char *member : kBand)
                if (path.starts_with(std::string("char/") + member + "/"))
                {
                    if (settings::get(settings::kBand) == settings::kBandGh2x)
                        return true;
                    if (settings::get(settings::kBand) == settings::kBandGh2)
                        return false;
                }
            return settings::hd(path == kTrack ? settings::kHighway
                                : path.starts_with(kVenues) ? settings::kVenues : settings::kCharacters);
        }

        std::optional<ark::Made> make(const Discs &discs, const std::string &path, const std::string &twin)
        {
            Looks looks;
            auto converted = convert(discs, twin, path, looks);
            if (!converted)
            {
                std::cerr << "[gh2x] " << twin << " cannot be converted; the PS2's own stays" << std::endl;
                return std::nullopt;
            }
            addSkins(discs, path, twin, *converted, looks);
            for (const auto &[mat, shading] : looks)
                look::add(mat, shading);
            return ark::held(milo::write(*converted));
        }
    }

    void installGh2xModels()
    {
        const auto xbox = games::disc("gh2x");
        if (!xbox)
            return;
        // The luminance map every venue's PostProc names (all eight copies
        // are the same file), for the x360 shading's post stage.
        look::setLuminance(textures::add({*xbox, kLuminanceMap, ""}));
        // Only the two discs' file tables and guitar lists are read here.
        auto discs = std::make_shared<Discs>();
        discs->xbox = *xbox;
        discs->ps2 = ark::paths(0u, "");
        std::vector<std::pair<std::string, std::string>> scenes; // 360 path, PS2 path
        for (const char *folder : kFolders)
            for (const std::string &twin : ark::paths(*xbox, folder))
                if (twin.ends_with(".milo_xbox"))
                    scenes.emplace_back(twin, gh2x::ps2Path(twin));
        scenes.emplace_back(kTrackTwin, kTrack);
        discs->any = discs->ps2;
        for (const auto &scene : scenes)
            discs->any.push_back(scene.second);
        std::sort(discs->any.begin(), discs->any.end());

        const auto theirs = readSkins(*xbox);
        for (const auto &[key, ours] : readSkins(0u))
        {
            const auto other = theirs.find(key);
            if (other != theirs.end())
                discs->skins[ours.first].push_back({ours.second, other->second.first, other->second.second});
        }

        for (const auto &[twin, path] : scenes)
        {
            auto made = [discs, path, twin] { return make(*discs, path, twin); };
            if (holds(discs->ps2, path))
                ark::addMade(0u, path, std::move(made), [path] { return wanted(path); });
            else
                ark::addMade(path, std::move(made));
        }
    }
}
