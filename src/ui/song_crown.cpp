// song_crown: the full combo crown on the song lists' rows. A row is a
// UIList element of ui_objects.dta's type song (the career's list) or song2
// (quickplay's), each of whose slots is a Text or a Mesh of its scene,
// list_song.milo or list_song2.milo, filled by SongProvider. Each type gains
// an eighth slot, crown.mesh, and each disc's scenes the crown's Tex, Mat and
// Mesh: the checkmark's, its bitmap the crown (tools/procedural/crown.py), set
// between the stars and the score. A mesh slot shows the Mat its provider
// gives it, and nothing for none (MeshSlot::Fill, 0x377f38).

#include "ui/song_crown.h"

#include "crown_bitmap.h"
#include "disc/ark.h"
#include "formats/dtb.h"
#include "gh1/scene.h"
#include "guest.h"
#include "milo/milo.h"
#include "save/save.h"
#include "script.h"

#include "ps2_runtime_macros.h"

#include <cstring>
#include <iostream>

namespace gh2
{
    namespace
    {
        const Addresses *s_addresses = nullptr;
        PS2Runtime::RecompiledFunction s_mat = nullptr;

        constexpr uint32_t kCrownSlot = 7u;
        // SongProvider's list scene (InitData, 0x117448); PlayerConfig's
        // difficulty (GetDifficulty, 0x113c18).
        constexpr uint32_t kListDir = 0x5cu;
        constexpr uint32_t kDifficulty = 0x2cu;
        // As SongProvider::Mat casts what FindObject gives it to a RndMat
        // (0x117940): __dynamic_cast from Hmx::Object's type to RndMat's.
        constexpr uint32_t kObjectType = 0x35bdd8u;
        constexpr uint32_t kMatType = 0x3c8ab0u;

        // Its middle in the row, between the stars (to 140) and the score
        // (right-aligned at 250), and its size, in the row's units; the quad
        // is 32 by 32.
        constexpr float kX = 157.0f, kSide = 26.0f;

        // A row's Mat for slot 7: the crown where the row's song is full
        // combo'd on player 1's difficulty, the career's in a career, the one
        // last picked in quickplay.
        void mat(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (GPR_U32(ctx, 7) != kCrownSlot)
                return s_mat(rdram, ctx, runtime);
            const uint32_t returnTo = GPR_U32(ctx, 31);
            const uint32_t provider = GPR_U32(ctx, 4), row = GPR_U32(ctx, 6);
            const auto call = [&](uint32_t function, std::initializer_list<uint32_t> args)
            { return static_cast<uint32_t>(runtime->callGuestFunction(rdram, ctx, function, args)); };
            uint32_t crown = 0u;
            const uint32_t dir = load<uint32_t>(rdram, provider + kListDir);
            const uint32_t song = dir ? call(s_addresses->songProviderGetSong, {provider, row}) : 0u;
            const uint32_t player = song ? call(s_addresses->gameConfigPlayerConfig,
                                                {load<uint32_t>(rdram, s_addresses->theGameConfig), 0u})
                                         : 0u;
            if (player && save::hasFullCombo(reinterpret_cast<const char *>(getMemPtr(rdram, song)),
                                             static_cast<int>(load<uint32_t>(rdram, player + kDifficulty))))
                if (const uint32_t object =
                        call(s_addresses->objectDirFindObject, {dir, script::symbol(rdram, ctx, runtime, "crown.mat"), 0u}))
                {
                    const uint32_t vtable = load<uint32_t>(rdram, object);
                    const uint32_t whole = object + static_cast<uint32_t>(static_cast<int32_t>(load<int16_t>(rdram, vtable)));
                    crown = call(s_addresses->dynamicCast,
                                 {load<uint32_t>(rdram, vtable + 4u), kObjectType, 1u, whole, kMatType, object});
                }
            SET_GPR_U32(ctx, 2, crown);
            ctx->pc = returnTo;
        }

        // The type's element, colours and provider lists, each a slot
        // longer: a mesh, the gold stars' colours, the Mat's name.
        void addSlot(dtb::Node &types, const char *name)
        {
            dtb::Node *type = const_cast<dtb::Node *>(dtb::find(types, name));
            dtb::Node *element = type ? const_cast<dtb::Node *>(dtb::find(*type, "element")) : nullptr;
            dtb::Node *colors = type ? const_cast<dtb::Node *>(dtb::find(*type, "colors")) : nullptr;
            dtb::Node *provider = type ? const_cast<dtb::Node *>(dtb::find(*type, "provider")) : nullptr;
            if (!element || !colors || !provider || element->nodes.size() < 2u || colors->nodes.size() < 2u ||
                provider->nodes.size() < 2u || element->nodes[1].nodes.size() != kCrownSlot ||
                colors->nodes[1].nodes.size() != kCrownSlot || provider->nodes[1].nodes.size() != kCrownSlot)
            {
                std::cerr << "[song_crown] UIList " << name << " is not as GH2 has it" << std::endl;
                return;
            }
            dtb::Node mesh = element->nodes[1].nodes[3];
            mesh.nodes[1].text = "crown.mesh";
            element->nodes[1].nodes.push_back(std::move(mesh));
            colors->nodes[1].nodes.push_back(colors->nodes[1].nodes[3]);
            dtb::Node mat = provider->nodes[1].nodes[3];
            mat.text = "crown.mat";
            provider->nodes[1].nodes.push_back(std::move(mat));
        }

        void setFloat(milo::Bytes &b, size_t at, float value)
        {
            std::memcpy(b.data() + at, &value, 4u);
        }

        float getFloat(const milo::Bytes &b, size_t at)
        {
            float value;
            std::memcpy(&value, b.data() + at, 4u);
            return value;
        }

        // The checkmark's Tex, Mat and Mesh again as the crown's.
        void addCrown(milo::Dir &scene, const std::string &path)
        {
            if (milo::find(scene, "crown.mesh"))
                return;
            const milo::Bytes *tex = milo::body(scene, "Tex", "checkmark.tex");
            const milo::Bytes *mat = milo::body(scene, "Mat", "checkmark.mat");
            const milo::Bytes *mesh = milo::body(scene, "Mesh", "checkmark.mesh");
            if (!tex || !mat || !mesh)
            {
                std::cerr << "[song_crown] no checkmark to copy in " << path << std::endl;
                return;
            }
            milo::Bytes crownTex = gh1::texWith(*tex, milo::Bytes(std::begin(kCrown), std::end(kCrown)), true);
            milo::Bytes crownMat = *mat, crownMesh = *mesh;
            // A Mesh has Hmx::Object's header, then its Trans 9: a revision,
            // then its local and world, each a 3x3 and a position, z up.
            const size_t trans = gh1::headerEnd(crownMesh, 4u).value_or(0u);
            if (crownTex.empty() || !milo::replaceStr(crownMat, "checkmark.tex", "crown.tex") ||
                !milo::replaceStr(crownMesh, "checkmark.mat", "crown.mat") || trans == 0u ||
                trans + 100u > crownMesh.size() || getFloat(crownMesh, trans + 4u + 36u) != -270.0f)
            {
                std::cerr << "[song_crown] the checkmark in " << path << " is not as GH2 has it" << std::endl;
                return;
            }
            for (const size_t xfm : {trans + 4u, trans + 52u})
            {
                setFloat(crownMesh, xfm, kSide / 32.0f);       // x axis
                setFloat(crownMesh, xfm + 32u, kSide / 32.0f); // z axis
                setFloat(crownMesh, xfm + 36u, kX);
            }
            milo::add(scene, "Tex", "crown.tex", std::move(crownTex));
            milo::add(scene, "Mat", "crown.mat", std::move(crownMat));
            milo::add(scene, "Mesh", "crown.mesh", std::move(crownMesh));
        }
    }

    void installSongCrown(PS2Runtime &runtime, const Addresses &addresses)
    {
        s_addresses = &addresses;
        ark::addFix(0u, "ui/gen/ui_objects.dtb", [](std::vector<uint8_t> &file)
                    {
                        auto root = dtb::raw(file);
                        dtb::Node *types = root ? const_cast<dtb::Node *>(dtb::find(*root, "UIList")) : nullptr;
                        types = types ? const_cast<dtb::Node *>(dtb::find(*types, "types")) : nullptr;
                        if (!types)
                            return;
                        addSlot(*types, "song");
                        addSlot(*types, "song2");
                        file = dtb::write(*root);
                    });
        // Every disc's: an 80s career's lists are its own scenes, of GH2's
        // types.
        for (size_t disc = 0u; disc < ark::discs(); ++disc)
            for (const std::string path : {"ui/gen/list_song.milo_ps2", "ui/gen/list_song2.milo_ps2"})
                if (!ark::paths(disc, path).empty())
                    ark::addFix(disc, path, [path](std::vector<uint8_t> &file)
                                {
                                    auto scene = milo::read(file);
                                    if (!scene)
                                        return;
                                    addCrown(*scene, path);
                                    file = milo::write(*scene);
                                });
        s_mat = runtime.lookupFunction(addresses.songProviderMat);
        runtime.replaceFunction(addresses.songProviderMat, &mat);
    }
}
