// GH1's guitarists in GH2, each as one more outfit of its character.
//
// GH1 keeps a guitarist as one scene (charsys/<x>/gen/<x>.rnd, a v10 milo)
// of Tex 8, Mat 21 and Mesh 25 objects, its bones Meshes named bone_*.mesh.
// GH2's outfit for the same character (a v24 BandCharacter) has Trans bones
// by those same names plus everything that makes a character run: drivers,
// IK, hair, eyes, lip sync. The outfit here is GH2's, its skin meshes swapped
// for GH1's objects as they are, which GH2's loaders still read (RndMesh::Load
// 0x3d5420 and RndMat::Load 0x1bfc00 keep paths for those revisions), its
// LOD groups listing GH1's meshes from GH1's own lod views, and its bones at
// GH1's rest. The picker plays GH1's own idle, converted from GH1's clips;
// songs still play GH2's base clips. GH1's face morphs come along from its
// face scene (<x>_face.rnd) for gh1/face to pose.

#include "gh1/install.h"

#include "disc/ark.h"
#include "content/outfits.h"
#include "gh1/clips.h"
#include "gh1/dtb.h"
#include "gh1/face.h"
#include "gh1/guitarist.h"
#include "gh1/rig.h"
#include "milo/milo.h"

#include <chrono>
#include <iostream>

namespace gh2
{
    namespace
    {
        using gh1::graft;
        using gh1::Guitarist;
        using gh1::load;
        using gh1::pickerSet;

        // GH1's locale names the folders: hair_metal Izzy, nu_metal Pandora,
        // hiphop Xavier.
        constexpr Guitarist kGuitarists[] = {
            // GH1's punk idles leaning on the back wall, where the door opens.
            {.gh1Folder = "punk", .gh2Character = "punk", .baseOutfit = "punk1", .label = "GH1 MOHAWK",
             .pickerDoorOpens = false},
            {.gh1Folder = "alterna", .gh2Character = "alterna", .baseOutfit = "alterna1", .label = "GH1 SKULLS"},
            {.gh1Folder = "metal", .gh2Character = "metal", .baseOutfit = "metal1", .label = "GH1 SHIRT"},
            {.gh1Folder = "hair_metal", .gh2Character = "glam", .baseOutfit = "glam1", .label = "GH1 CODPIECE",
             .clipPrefix = "hair", .highway = "hair"},
            {.gh1Folder = "nu_metal", .gh2Character = "goth", .baseOutfit = "goth2", .label = "GH1 LEATHERS",
             .clipPrefix = "nu"},
            {.gh1Folder = "hiphop", .gh2Character = "funk1", .baseOutfit = "funk1", .label = "GH1"},
            {.gh1Folder = "classic", .gh2Character = "classic", .baseOutfit = "classic", .label = "GH1"},
            {.gh1Folder = "grim", .gh2Character = "grim", .baseOutfit = "grim", .label = "GH1"},
        };

        // Characters whose one outfit the locale never named.
        constexpr const char *kUnnamed[] = {"funk1", "classic", "grim"};

        // GH1's archetypes (charsys.dta), with the macros GH1 loads at boot
        // first.
        std::optional<gh1::dtb::Node> archetypes(size_t gh1Disc)
        {
            gh1::dtb::Macros macros;
            const auto macroFile = ark::readFile(gh1Disc, "../../system/run/config/gen/macros.dtb");
            const auto charsysFile = ark::readFile(gh1Disc, "charsys/gen/charsys.dtb");
            if (!macroFile || !charsysFile || !gh1::dtb::read(*macroFile, macros))
                return std::nullopt;
            const auto charsys = gh1::dtb::read(*charsysFile, macros);
            if (!charsys)
                return std::nullopt;
            const gh1::dtb::Node *found = gh1::dtb::find(*charsys, "archetypes");
            return found ? std::optional(*found) : std::nullopt;
        }

        // A scene as GH1's scripts name it (charsys/metal/metal_face.rnd)
        // where its built file is (charsys/metal/gen/metal_face.rnd_ps2).
        std::string built(const std::string &path)
        {
            const size_t slash = path.rfind('/');
            return path.substr(0, slash) + "/gen/" + path.substr(slash + 1) + "_ps2";
        }
    }

    void installGh1()
    {
        const auto gh1Disc = ark::discWithSerial("SLUS_212.24");
        const auto gh2Disc = ark::discWithSerial("SLUS_214.47");
        if (!gh1Disc || !gh2Disc)
            return;
        const auto start = std::chrono::steady_clock::now();
        const auto types = archetypes(*gh1Disc);
        if (!types)
        {
            std::cerr << "[gh1] cannot read GH1's charsys" << std::endl;
            return;
        }
        size_t count = 0u;
        for (const Guitarist &guitarist : kGuitarists)
        {
            const std::string folder = guitarist.gh1Folder, base = guitarist.baseOutfit, outfit = guitarist.outfit();
            // CharFace reads both from the archetype (GH1 0x2a6cb0).
            const gh1::dtb::Node *archetype = gh1::dtb::find(*types, folder);
            const gh1::dtb::Node *faceFile = archetype ? gh1::dtb::find(*archetype, "face_file") : nullptr;
            const gh1::dtb::Node *faceData = archetype ? gh1::dtb::find(*archetype, "face_data") : nullptr;
            if (!faceFile || faceFile->nodes.size() < 2u || !faceData || !gh1::addFace(outfit, *faceData))
            {
                std::cerr << "[gh1] cannot read " << folder << "'s face" << std::endl;
                continue;
            }
            const auto gh1 = load("charsys/" + folder + "/gen/" + folder + ".rnd_ps2");
            const auto face = load(built(faceFile->nodes[1].text));
            const auto gh2 = load("char/" + base + "/og/gen/" + base + ".milo_ps2");
            const auto gh2Ui = load("char/" + base + "/og/gen/" + base + "_ui.milo_ps2");
            // The base's _ui plays its picker clips by a path relative to
            // itself: ../../anims/metal1_ui.milo, ../../../goth1/anims/goth1_ui.milo.
            const auto uiClips = gh2Ui ? milo::findSuffix(*gh2Ui, "_ui.milo") : std::nullopt;
            if (!gh1 || !face || !gh2 || !uiClips)
            {
                std::cerr << "[gh1] cannot read " << folder << " or " << base << std::endl;
                continue;
            }
            const std::string file = uiClips->substr(uiClips->rfind('/') + 1);
            const auto picker = pickerSet(guitarist, file.substr(0, file.size() - 8u), *gh1);
            if (!picker)
            {
                std::cerr << "[gh1] cannot build " << folder << "'s picker clips" << std::endl;
                continue;
            }
            milo::Dir ui = graft(*gh2Ui, *gh1, *face, outfit);
            milo::replacePrefix(ui, *uiClips, "../../../" + outfit + "/anims/" + outfit + "_ui.milo");
            ark::addFile("char/" + outfit + "/og/gen/" + outfit + ".milo_ps2",
                         milo::write(graft(*gh2, *gh1, *face, outfit)));
            ark::addFile("char/" + outfit + "/og/gen/" + outfit + "_ui.milo_ps2", milo::write(ui));
            ark::addFile("char/" + outfit + "/anims/gen/" + outfit + "_ui.milo_ps2", *picker);
            // _horse and anything else beside it is GH2's base outfit's, and
            // so are the photos. The highway (track/surfaces/%s_keep.bmp) is
            // GH1's, which GH2's loader reads as its own.
            ark::rename("char/" + outfit + "/og/gen/" + outfit, *gh2Disc, "char/" + base + "/og/gen/" + base);
            ark::rename("track/surfaces/gen/" + outfit + "_keep", *gh1Disc, "track/surfaces/gen/" + guitarist.track());
            outfits::photosFrom(outfit, *gh2Disc, base);
            outfits::add(guitarist.gh2Character, outfit, base, guitarist.label);
            ++count;
        }
        for (const char *character : kUnnamed)
            outfits::label(character, character, "CLASSIC");
        const auto ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
        std::cerr << "[gh1] " << count << " outfits built in " << ms << " ms" << std::endl;
    }
}
