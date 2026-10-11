#include "content/hidden_guitars.h"

#include "content/campaigns.h"
#include "content/locale.h"
#include "disc/ark.h"
#include "dta.h"
#include "formats/dtb.h"
#include "gh1/rig.h"
#include "milo/milo.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iostream>
#include <optional>

namespace gh2
{
    namespace
    {
        using dtb::array;
        using dtb::symbol;

        struct Finish
        {
            const char *guitar, *key, *outfit, *mat;
            int32_t price;
        };
        constexpr Finish kFinishes[] = {
            {"explorer", "exp_peppermint", "xplorer", "guitar_xplorer_peppermint.mat", 450},
            {"explorer", "exp_lipstick", "xplorer", "guitar_xplorer_lipstick.mat", 550},
        };
        // The controller costs more than any guitar the disc sells (5000),
        // and is earned as this reward guitar is.
        constexpr const char *kController = "guitar_red_octane", *kEarnedAs = "snaketapus_reward";
        constexpr int32_t kControllerPrice = 6000;
        constexpr const char *kAir = "air_guitar", *kAirMat = "guitar_lespaul_burst.mat";
        constexpr int32_t kAirPrice = 1000;

        dtb::Node *child(dtb::Node &parent, const std::string &key)
        {
            for (dtb::Node &n : parent.nodes)
                if (n.type == dtb::kArray && !n.nodes.empty() && n.nodes[0].text == key)
                    return &n;
            return nullptr;
        }

        dtb::Node priced(const std::string &key, int32_t price)
        {
            return array({symbol(key), array({symbol("price"), {dtb::kInt, price, 0.0f, {}, {}}})});
        }

        dtb::Node skin(const char *key, const char *outfit, const char *mat)
        {
            return array({symbol(key), array({symbol("outfit"), symbol(outfit)}), array({symbol("mat"), symbol(mat)})});
        }

        // A skin's Mat goes on its scene's guitar.mesh, which the air
        // guitar's scene has none of: the cheat swaps the scene in with no
        // skin. It gets another guitar's, not showing. A Mesh 28 is 17 bytes,
        // a Trans's two transforms, four bytes, its target, a byte and its
        // parent, then its Draw's rev and whether it shows.
        constexpr const char *kScenes = "char/og/guitars/gen/", *kMesh = "guitar.mesh", *kMeshFrom = "guitar_sg";
        constexpr const char *kControllerScene = "guitar_redoctane";
        constexpr size_t kMeshLocal = 17u, kMeshWorld = 65u;

        // A Mesh 28's parent, past its transforms.
        std::string parentOf(const milo::Bytes &mesh)
        {
            size_t o = kMeshWorld + 48u + 4u;
            if (mesh.size() < o + 8u)
                return {};
            milo::str(mesh, o);
            o += 1u;
            return o + 4u <= mesh.size() ? milo::str(mesh, o) : std::string();
        }

        // Where a Mesh's verts are and how many: a count, that many of 48
        // bytes (a place, then a normal), then a count of faces of three
        // shorts, each short of those verts. The largest such run in it.
        std::optional<std::pair<size_t, size_t>> verts(const milo::Bytes &mesh)
        {
            std::optional<std::pair<size_t, size_t>> best;
            for (size_t o = kMeshWorld + 48u; o + 8u <= mesh.size(); ++o)
            {
                const size_t count = milo::u32(mesh, o), end = o + 4u + count * 48u;
                if (count < 3u || count > 20000u || end + 4u > mesh.size() || (best && count <= best->second))
                    continue;
                const size_t faces = milo::u32(mesh, end);
                if (faces == 0u || faces > 40000u || end + 4u + faces * 6u > mesh.size())
                    continue;
                bool within = true;
                for (size_t i = 0; within && i < faces * 3u; ++i)
                    within = (mesh[end + 4u + i * 2u] | mesh[end + 5u + i * 2u] << 8) < static_cast<int>(count);
                if (within)
                    best = std::make_pair(o + 4u, count);
            }
            return best;
        }

        // The controller's scene is GH1's in GH2's form, its body turned
        // about the neck from how GH2's guitars stand under the guitar spot
        // (gh1/rig.cpp's guitar): in a hand it is right, and on show, in the
        // store and the picker, it faces away. Its guitar.mesh takes
        // `stand`'s turn, its verts moved by the difference, and what hangs
        // off it moves by the same to stay where it was.
        bool faceFront(milo::Dir &scene, const gh1::Xfm &stand)
        {
            const auto at = milo::find(scene, kMesh);
            const auto run = at ? verts(scene.bodies[*at]) : std::nullopt;
            if (!run)
                return false;
            milo::Bytes &mesh = scene.bodies[*at];
            const gh1::Xfm was = gh1::xfm(mesh, kMeshLocal);
            gh1::Xfm stood = was;
            std::copy(stand.m, stand.m + 9, stood.m);
            const gh1::Xfm by = was * gh1::inverse(stood);
            for (size_t i = 0; i < run->second; ++i)
                for (const bool place : {true, false})
                {
                    const size_t o = run->first + i * 48u + (place ? 0u : 12u);
                    float v[3], r[3];
                    std::memcpy(v, mesh.data() + o, 12u);
                    for (int j = 0; j < 3; ++j)
                        r[j] = (place ? by.v[j] : 0.0f) + v[0] * by.m[j] + v[1] * by.m[3 + j] + v[2] * by.m[6 + j];
                    if (const float length = std::sqrt(r[0] * r[0] + r[1] * r[1] + r[2] * r[2]); !place && length > 0.0f)
                        for (float &f : r)
                            f /= length;
                    std::memcpy(mesh.data() + o, r, 12u);
                }
            const milo::Bytes local = gh1::bytes(stood), world = gh1::bytes(stood * gh1::inverse(was) * gh1::xfm(mesh, kMeshWorld));
            std::copy(local.begin(), local.end(), mesh.begin() + kMeshLocal);
            std::copy(world.begin(), world.end(), mesh.begin() + kMeshWorld);
            for (size_t i = 0; i < scene.entries.size(); ++i)
                if (scene.entries[i].first == "Mesh" && i != *at && parentOf(scene.bodies[i]) == kMesh)
                {
                    const milo::Bytes moved = gh1::bytes(gh1::xfm(scene.bodies[i], kMeshLocal) * by);
                    std::copy(moved.begin(), moved.end(), scene.bodies[i].begin() + kMeshLocal);
                }
            return true;
        }
        std::optional<milo::Bytes> hiddenMesh()
        {
            const auto file = ark::readFile(0u, std::string(kScenes) + kMeshFrom + ".milo_ps2");
            const auto scene = file ? milo::read(*file) : std::nullopt;
            const auto at = scene ? milo::find(*scene, kMesh) : std::nullopt;
            if (!at)
                return std::nullopt;
            milo::Bytes mesh = scene->bodies[*at];
            size_t o = 17u + 96u + 4u;
            if (mesh.size() < o + 16u)
                return std::nullopt;
            milo::str(mesh, o);
            o += 1u;
            milo::str(mesh, o);
            if (o + 5u > mesh.size() || milo::u32(mesh, o) > 8u)
                return std::nullopt;
            mesh[o + 4u] = 0u;
            return mesh;
        }
    }

    bool faceGuitarFront(milo::Dir &scene, const gh1::Xfm &stand)
    {
        return faceFront(scene, stand);
    }

    void installHiddenGuitars()
    {
        if (const auto copy = dtb::parse(kGuitarCopyDta))
            for (const dtb::Node &line : copy->nodes)
                if (line.type == dtb::kArray && line.nodes.size() > 1u && !line.nodes[1].text.empty())
                    locale::add(line.nodes[0].text, line.nodes[1].text);

        const dtb::Files game = [](const std::string &path) { return ark::readFile(0u, path); };
        dtb::Macros none;
        auto guitars = dtb::read("config/guitars.dta", none, game);
        auto store = dtb::read("config/store.dta", none, game);
        dtb::Node *sold = store ? child(*store, "guitar") : nullptr, *finishes = store ? child(*store, "skin") : nullptr;
        dtb::Node *controller = guitars ? child(*guitars, kController) : nullptr;
        dtb::Node *earned = guitars ? child(*guitars, kEarnedAs) : nullptr;
        if (!sold || !finishes || !controller || !earned)
        {
            std::cerr << "[guitars] cannot read the game disc's guitars and store" << std::endl;
            return;
        }
        for (const Finish &f : kFinishes)
            if (dtb::Node *skins = child(*guitars, f.guitar) ? child(*child(*guitars, f.guitar), "skins") : nullptr)
            {
                skins->nodes.push_back(skin(f.key, f.outfit, f.mat));
                finishes->nodes.push_back(priced(f.key, f.price));
            }
        // A guitar, in stock once its tour is passed (StoreGuitarProvider::
        // InStock, 0x119a70).
        std::vector<dtb::Node> terms;
        for (const char *key : {"require", "difficulty"})
            if (const dtb::Node *term = child(*earned, key))
                terms.push_back(*term);
        if (dtb::Node *type = child(*controller, "type"); type && type->nodes.size() > 1u)
            type->nodes[1] = symbol("guitar");
        controller->nodes.insert(controller->nodes.begin() + 2, terms.begin(), terms.end());
        sold->nodes.push_back(priced(kController, kControllerPrice));
        {
            const std::string path = std::string(kScenes) + kControllerScene + ".milo_ps2";
            const auto file = ark::readFile(0u, path), like = ark::readFile(0u, std::string(kScenes) + kMeshFrom + ".milo_ps2");
            auto scene = file ? milo::read(*file) : std::nullopt;
            const auto ours = like ? milo::read(*like) : std::nullopt;
            const auto mesh = ours ? milo::find(*ours, kMesh) : std::nullopt;
            if (scene && mesh && faceFront(*scene, gh1::xfm(ours->bodies[*mesh], kMeshLocal)))
                ark::addFile(path, milo::write(*scene));
            else
                std::cerr << "[guitars] cannot turn the controller to face the front" << std::endl;
        }

        const std::string airPath = std::string(kScenes) + kAir + ".milo_ps2";
        const auto airFile = ark::readFile(0u, airPath);
        auto air = airFile ? milo::read(*airFile) : std::nullopt;
        if (const auto mesh = hiddenMesh(); air && mesh && !milo::find(*air, kMesh))
        {
            milo::add(*air, "Mesh", kMesh, *mesh);
            ark::addFile(airPath, milo::write(*air));
            guitars->nodes.push_back(array({symbol(kAir), array({symbol("type"), symbol("guitar")}),
                                            array({symbol("skins"), skin(kAir, kAir, kAirMat)})}));
            sold->nodes.push_back(priced(kAir, kAirPrice));
        }
        else
            std::cerr << "[guitars] cannot make the air guitar a guitar to pick" << std::endl;

        const size_t layer = ark::addLayer(std::nullopt);
        ark::addFile(layer, "config/gen/guitars.dtb", dtb::write(*guitars));
        ark::addFile(layer, "config/gen/store.dtb", dtb::write(*store));
        campaigns::over(layer);
    }
}
