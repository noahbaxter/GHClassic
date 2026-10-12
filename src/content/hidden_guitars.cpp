#include "content/hidden_guitars.h"

#include "content/campaigns.h"
#include "content/locale.h"
#include "content/tour_or_higher.h"
#include "disc/ark.h"
#include "dta.h"
#include "formats/dtb.h"
#include "gh1/rig.h"
#include "gh1/scene.h"
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
        // and is earned as this reward guitar is, on hard (2) or above.
        constexpr const char *kController = "guitar_red_octane", *kEarnedAs = "snaketapus_reward";
        constexpr int32_t kControllerPrice = 6000, kControllerFrom = 2;
        // GH2's own controller, which no disc has: the first game's with its
        // black plastic red, its white pickguard black and its neck black,
        // painted from the disc's texture as a finish of it.
        constexpr const char *kRed = "guitar_red_octane_gh2", *kRedMat = "guitar_redoctane_gh2.mat";
        constexpr const char *kRedTex = "redoctane_gh2.tex", *kControllerTex = "redoctane.tex";
        constexpr const char *kControllerMat = "guitar_redoctane.mat";
        constexpr int32_t kRedPrice = 550;
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

        // The controller's Tex as GH2's controller is coloured. Its bitmap
        // is 32 bytes, 256 colours of four (the PS2's order: indices with
        // bit 3 or 4 alone set swap), then a byte a texel, 256 by 128. The
        // black plastic and the neck's brown change wherever they are, the strum
        // bar's surround with them. Three parts of the picture differ, so
        // their texels take colours of their own from the unused ones: the
        // pickguard, whose white goes black; the whammy bar's tip, which
        // stays black; and the neck, where the pale edges of the fret
        // buttons go black with it. Each is left, top, right, bottom.
        constexpr size_t kBitmapHead = 32u, kColours = 256u, kTexWide = 256u, kTexHigh = 128u;
        constexpr size_t kGuard[4] = {48u, 50u, 138u, 81u};
        constexpr size_t kTip[4] = {0u, 56u, 17u, 96u}, kNeck[4] = {0u, 0u, 255u, 23u};
        bool within(const size_t box[4], size_t x, size_t y)
        {
            return x >= box[0] && x <= box[2] && y >= box[1] && y <= box[3];
        }
        std::optional<milo::Bytes> redController(const milo::Bytes &tex)
        {
            const size_t size = kBitmapHead + kColours * 4u + kTexWide * kTexHigh;
            if (tex.size() < size || tex[tex.size() - size + 1u] != 8u)
                return std::nullopt;
            milo::Bytes out = tex;
            uint8_t *palette = out.data() + out.size() - size + kBitmapHead, *texels = palette + kColours * 4u;
            const auto slot = [&](size_t index) { return palette + ((index & 0x18u) == 8u || (index & 0x18u) == 16u ? index ^ 0x18u : index) * 4u; };
            bool used[kColours] = {};
            for (size_t i = 0; i < kTexWide * kTexHigh; ++i)
                used[texels[i]] = true;
            // In those parts a colour goes to a spare one, kept as it was or
            // made black: 0 for no part, 1 kept, 2 black.
            int spare[3][kColours];
            std::fill(&spare[0][0], &spare[0][0] + 3 * kColours, -1);
            size_t next = 0u;
            for (size_t y = 0; y < kTexHigh; ++y)
                for (size_t x = 0; x < kTexWide; ++x)
                {
                    uint8_t &texel = texels[y * kTexWide + x];
                    const uint8_t *from = slot(texel);
                    const int high = std::max({from[0], from[1], from[2]}), low = std::min({from[0], from[1], from[2]});
                    int as = 0;
                    // The body's black around the pickguard is in its box
                    // too, and goes red as the rest does.
                    if (within(kGuard, x, y))
                        as = high >= 90 ? 2 : 0;
                    else if (within(kTip, x, y))
                        as = 1;
                    else if (within(kNeck, x, y) && high - low < 60 && high > 60)
                        as = 2;
                    if (as == 0)
                        continue;
                    if (spare[as][texel] < 0)
                    {
                        while (next < kColours && used[next])
                            ++next;
                        if (next == kColours)
                            return std::nullopt;
                        uint8_t *to = slot(next);
                        std::copy(from, from + 4, to);
                        if (as == 2)
                            to[0] = to[1] = to[2] = static_cast<uint8_t>(6 + low / 24);
                        spare[as][texel] = static_cast<int>(next++);
                    }
                    texel = static_cast<uint8_t>(spare[as][texel]);
                }
            for (size_t i = 0; i < kColours; ++i)
            {
                if (!used[i])
                    continue;
                uint8_t *c = slot(i);
                const int high = std::max({c[0], c[1], c[2]}), low = std::min({c[0], c[1], c[2]});
                const int light = (c[0] * 299 + c[1] * 587 + c[2] * 114) / 1000;
                if (high < 90 && high - low < 30)
                {
                    c[0] = static_cast<uint8_t>(120 + 110 * light / 90);
                    c[1] = static_cast<uint8_t>(8 + 40 * light / 90);
                    c[2] = static_cast<uint8_t>(14 + 44 * light / 90);
                }
                else if (c[0] > c[1] && c[1] > c[2] && high - low > 30 && high - low < high * 6 / 10 && c[1] * 2 > c[0])
                    c[0] = c[1] = c[2] = static_cast<uint8_t>(light * 16 / 100);
            }
            return out;
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

    bool offerHiddenGuitars(dtb::Node &guitars, dtb::Node &store)
    {
        dtb::Node *sold = child(store, "guitar"), *finishes = child(store, "skin");
        dtb::Node *controller = child(guitars, kController), *earned = child(guitars, kEarnedAs);
        if (!sold || !finishes || !controller || !earned)
            return false;
        for (const Finish &f : kFinishes)
            if (dtb::Node *skins = child(guitars, f.guitar) ? child(*child(guitars, f.guitar), "skins") : nullptr)
            {
                skins->nodes.push_back(skin(f.key, f.outfit, f.mat));
                finishes->nodes.push_back(priced(f.key, f.price));
            }
        // A guitar, in stock once hard's tour or expert's is passed
        // (StoreGuitarProvider::InStock, 0x119a70).
        std::vector<dtb::Node> terms;
        if (const dtb::Node *term = child(*earned, "require"))
            terms.push_back(*term);
        terms.push_back(array({symbol("difficulty"), {dtb::kInt, kControllerFrom | kOrHigher, 0.0f, {}, {}}}));
        if (dtb::Node *type = child(*controller, "type"); type && type->nodes.size() > 1u)
            type->nodes[1] = symbol("guitar");
        controller->nodes.insert(controller->nodes.begin() + 2, terms.begin(), terms.end());
        sold->nodes.push_back(priced(kController, kControllerPrice));
        if (dtb::Node *skins = child(*controller, "skins"))
        {
            skins->nodes.push_back(skin(kRed, kControllerScene, kRedMat));
            finishes->nodes.push_back(priced(kRed, kRedPrice));
        }
        guitars.nodes.push_back(array({symbol(kAir), array({symbol("type"), symbol("guitar")}),
                                       array({symbol("skins"), skin(kAir, kAir, kAirMat)})}));
        sold->nodes.push_back(priced(kAir, kAirPrice));
        return true;
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
        if (!guitars || !store || !offerHiddenGuitars(*guitars, *store))
        {
            std::cerr << "[guitars] cannot read the game disc's guitars and store" << std::endl;
            return;
        }
        // The scenes are served ahead of every disc, so whichever game is
        // played has them.
        {
            const std::string path = std::string(kScenes) + kControllerScene + ".milo_ps2";
            const auto file = ark::readFile(0u, path), like = ark::readFile(0u, std::string(kScenes) + kMeshFrom + ".milo_ps2");
            auto scene = file ? milo::read(*file) : std::nullopt;
            const auto ours = like ? milo::read(*like) : std::nullopt;
            const auto mesh = ours ? milo::find(*ours, kMesh) : std::nullopt;
            if (scene && mesh && faceFront(*scene, gh1::xfm(ours->bodies[*mesh], kMeshLocal)))
            {
                const auto tex = milo::find(*scene, kControllerTex), mat = milo::find(*scene, kControllerMat);
                if (const auto red = tex && mat ? redController(scene->bodies[*tex]) : std::nullopt)
                {
                    milo::add(*scene, "Tex", kRedTex, *red);
                    milo::add(*scene, "Mat", kRedMat, gh1::renamed(scene->bodies[*mat], kControllerTex, kRedTex));
                }
                else
                    std::cerr << "[guitars] cannot paint the controller as GH2's" << std::endl;
                ark::addFile(path, milo::write(*scene));
            }
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
        }
        else
            std::cerr << "[guitars] cannot make the air guitar a guitar to pick" << std::endl;

        const size_t layer = ark::addLayer(std::nullopt);
        ark::addFile(layer, "config/gen/guitars.dtb", dtb::write(*guitars));
        ark::addFile(layer, "config/gen/store.dtb", dtb::write(*store));
        campaigns::over(layer);
    }
}
