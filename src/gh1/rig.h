#pragma once

// GH1's rig in GH2's outfits: GH1's bones and transforms, and GH1's scene
// grafted onto GH2's outfit.

#include "milo/milo.h"

#include <map>
#include <optional>
#include <string>
#include <vector>

namespace gh2
{
    namespace gh1
    {
        using milo::Bytes;

        // GH1's Trans 8, inside its Mesh 25 at offset 4: rev, local, world,
        // then child names (GH1's hierarchy; its parent field names itself).
        struct Bone
        {
            Bytes world;
            std::vector<std::string> children;
        };

        // GH1's bones (bone_*.mesh) by name.
        std::map<std::string, Bone> gh1Bones(const milo::Dir &gh1);

        // Each child's parent, by GH1's child lists.
        std::map<std::string, std::string> owners(const std::map<std::string, Bone> &bones);

        // A transform as stored: three axis rows, then the position. Points
        // are rows: world = local * parent, as GH2's own bones check out.
        struct Xfm
        {
            float m[9];
            float v[3];
        };

        Xfm xfm(const Bytes &b, size_t o);
        Bytes bytes(const Xfm &x);
        Xfm operator*(const Xfm &a, const Xfm &b);
        Xfm inverse(const Xfm &a);

        // GH2's Trans 9: rev, object header, local, world, constraint,
        // target, preserve scale, parent.
        constexpr size_t kTransLocal = 13u, kTransWorld = 61u;

        // GH2's outfit `gh2` wearing GH1's scene `gh1` and its face scene.
        milo::Dir graft(const milo::Dir &gh2, const milo::Dir &gh1, const milo::Dir &face);

        // A scene from the mounted discs.
        std::optional<milo::Dir> load(const std::string &path);
    }
}
