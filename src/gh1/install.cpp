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
// GH1's rest. The picker and songs play GH1's own clips, converted into the
// base outfit's clip sets (gh1/clips.cpp). GH1's face morphs come along from
// its face scene (<x>_face.rnd) for gh1/face to pose.

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
        // hiphop Xavier. GH1's punk and hiphop idle leaning on the back wall,
        // where the door opens, so it stays shut behind them.
        constexpr Guitarist kGuitarists[] = {
            // character  label          base        folder        door opens
            {"punk",      "SAFETY PINS", "punk1",    "punk",       false},
            {"alterna",   "CORSET",      "alterna1", "alterna",    true},
            {"metal",     "SHORT",       "metal1",   "metal",      true},
            {"glam",      "BIGGER BOOT", "glam1",    "hair_metal", true},
            {"goth",      "RAZORS",      "goth2",    "nu_metal",   true},
            {"funk1",     "JADE",        "funk1",    "hiphop",     false},
            {"classic",   "BRIT",        "classic",  "classic",    true},
            {"grim",      "SCHEMIN'",    "grim",     "grim",       true},
        };

        // GH1's archetypes (charsys.dta), and the macros that defines (its
        // anim sets among them), after those GH1 loads at boot.
        struct Charsys
        {
            gh1::dtb::Node archetypes;
            gh1::dtb::Macros macros;
        };

        std::optional<Charsys> charsys(size_t gh1Disc)
        {
            const gh1::dtb::Files files = [gh1Disc](const std::string &path) { return ark::readFile(gh1Disc, path); };
            Charsys out;
            if (!gh1::dtb::read("../../system/run/config/macros.dta", out.macros, files))
                return std::nullopt;
            const auto root = gh1::dtb::read("charsys/charsys.dta", out.macros, files);
            const gh1::dtb::Node *found = root ? gh1::dtb::find(*root, "archetypes") : nullptr;
            if (!found)
                return std::nullopt;
            out.archetypes = *found;
            return out;
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
        const auto scripts = charsys(*gh1Disc);
        if (!scripts)
        {
            std::cerr << "[gh1] cannot read GH1's charsys" << std::endl;
            return;
        }
        const gh1::dtb::Node *types = &scripts->archetypes;
        size_t count = 0u;
        for (const Guitarist &guitarist : kGuitarists)
        {
            const std::string base = guitarist.base, name = guitarist.name(), folder = guitarist.folder;
            // CharFace reads both from the archetype (GH1 0x2a6cb0).
            const gh1::dtb::Node *archetype = gh1::dtb::find(*types, folder);
            const gh1::dtb::Node *faceFile = archetype ? gh1::dtb::find(*archetype, "face_file") : nullptr;
            const gh1::dtb::Node *faceData = archetype ? gh1::dtb::find(*archetype, "face_data") : nullptr;
            if (!faceFile || faceFile->nodes.size() < 2u || !faceData || !gh1::addFace(name, *faceData))
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
            const auto songClips = gh2 ? milo::findSuffix(*gh2, "_main.milo") : std::nullopt;
            if (!gh1 || !face || !gh2 || !uiClips || !songClips)
            {
                std::cerr << "[gh1] cannot read " << folder << " or " << base << std::endl;
                continue;
            }
            // Both name their clip sets char/<set>/anims/<set>_<kind>.milo.
            const auto setOf = [](const std::string &path, size_t kind)
            {
                const std::string file = path.substr(path.rfind('/') + 1);
                return file.substr(0, file.size() - kind);
            };
            const std::string uiSet = setOf(*uiClips, 8u), songSet = setOf(*songClips, 10u);
            const auto picker = pickerSet(guitarist, uiSet, *gh1);
            const auto songs = songSets(guitarist, songSet, *gh1, scripts->macros);
            if (!picker || !songs)
            {
                std::cerr << "[gh1] cannot build " << folder << "'s clips" << std::endl;
                continue;
            }
            milo::Dir main = graft(*gh2, *gh1, *face, name);
            milo::Dir ui = graft(*gh2Ui, *gh1, *face, name);
            const std::string ours = "../../../" + name + "/anims/" + name;
            const std::string base1 = songClips->substr(0, songClips->size() - 10u);
            for (milo::Dir *dir : {&main, &ui})
            {
                milo::replacePrefix(*dir, *uiClips, ours + "_ui.milo");
                for (const char *kind : {"_main.milo", "_fret.milo", "_strum.milo"})
                    milo::replacePrefix(*dir, base1 + kind, ours + kind);
            }
            ark::addFile("char/" + name + "/og/gen/" + name + ".milo_ps2", milo::write(main));
            ark::addFile("char/" + name + "/og/gen/" + name + "_ui.milo_ps2", milo::write(ui));
            ark::addFile("char/" + name + "/anims/gen/" + name + "_ui.milo_ps2", *picker);
            ark::addFile("char/" + name + "/anims/gen/" + name + "_main.milo_ps2", songs->main);
            ark::addFile("char/" + name + "/anims/gen/" + name + "_fret.milo_ps2", songs->fret);
            ark::addFile("char/" + name + "/anims/gen/" + name + "_strum.milo_ps2", songs->strum);
            // _horse and anything else beside it is GH2's base outfit's, and
            // so are the photos. The highway (track/surfaces/%s_keep.bmp) is
            // GH1's, which GH2's loader reads as its own: the folder's, or
            // its prefix's (hair.bmp, but nu_metal.bmp).
            const std::string surfaces = "track/surfaces/gen/";
            const std::string highway =
                ark::readFile(*gh1Disc, surfaces + folder + ".bmp_ps2") ? folder : guitarist.prefix();
            ark::rename("char/" + name + "/og/gen/" + name, *gh2Disc, "char/" + base + "/og/gen/" + base);
            ark::rename(surfaces + name + "_keep", *gh1Disc, surfaces + highway);
            outfits::photosFrom(name, *gh2Disc, base);
            outfits::add(guitarist.character, name, base, guitarist.label);
            ++count;
        }
        const auto ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
        std::cerr << "[gh1] " << count << " outfits built in " << ms << " ms" << std::endl;
    }
}
