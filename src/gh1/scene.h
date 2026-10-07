#pragma once

// GH1's scene objects as their bodies keep them.

#include "milo/milo.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace gh2::gh1
{
    using milo::Bytes;

    float f32(const Bytes &b, size_t o);
    void putF32(Bytes &out, float v);
    uint8_t u8(const Bytes &b, size_t o);
    void put(Bytes &out, const Bytes &b, size_t from, size_t to);

    // A count, then that many names.
    std::vector<std::string> names(const Bytes &b, size_t &o);
    void putNames(Bytes &out, const std::vector<std::string> &names);

    // A Trans 8 (RndTransformable::Load, GH1 0x310330): its local and
    // world, its children, then its constraint, target and preserve
    // scale, and its own name where a parent would be.
    struct Trans
    {
        size_t at = 0u;
        std::vector<std::string> children;
        size_t constraint = 0u, parent = 0u, end = 0u;
    };

    std::optional<Trans> trans(const Bytes &b, size_t &o);

    // A Draw 1 (RndDrawable::Load, GH1 0x30de18): whether it shows, what
    // it draws after itself, and a sphere.
    struct Draw
    {
        bool showing = false;
        std::vector<std::string> children;
        size_t sphere = 0u;
    };

    std::optional<Draw> draw(const Bytes &b, size_t &o);

    // An Anim 0 (RndAnimatable::Load, GH1 0x1b43b0): its filters, each a
    // kind and two floats (0 a scale and an offset; 1 a range, with a
    // flag more for whether it loops; 4 a float more), then the anims it
    // drives.
    struct Anim
    {
        float scale = 1.0f, offset = 0.0f, min = 0.0f, max = 0.0f;
        bool loop = false;
        std::vector<std::string> children;

        // Whether GH2 makes a filter of it (0x1ab494).
        bool filtered() const { return scale != 1.0f || offset != 0.0f || min != max; }
    };

    // The Anim 0 at `o`, which is left past it.
    std::optional<Anim> anim(const Bytes &b, size_t &o);

    // An Anim 4 (0x1ab2d8) in place of an Anim 0: its frame, and the
    // rate GH2's own menu anims have.
    void putAnim(Bytes &out);

    // GH2's name for the filter of an anim that has one.
    std::string filterOf(const std::string &name);

    // An AnimFilter 1 (RndAnimFilter::Load, 0x1acf48) of that anim:
    // Hmx::Object's header, an Anim 4, the anim, its scale, offset and
    // range, whether it loops, and no period.
    Bytes filter(const std::string &name, const Anim &of);

    // A MatAnim 5's stages (RndMatAnim::Load, GH1 0x1cb570): where each
    // starts and the last ends, and whether each has keys. A stage is
    // its translation, scale and rotation keys, each a vector and a
    // frame, then its texture keys, each a name and a frame.
    struct Stages
    {
        std::vector<size_t> at;
        std::vector<bool> keyed;
    };

    Stages stages(const Bytes &b, size_t o);

    // A name less its extension.
    std::string base(const std::string &name);

    // The anim a MatAnim's stage after the first is here: <anim>_<n>.mnm.
    std::string stageOf(const std::string &anim, size_t stage);

    // A body's revision, its first word.
    Bytes revision(const Bytes &b);

    // An object's parts every class here has, and where its own fields
    // start. A Text 15 (RndText::Load, GH1 0x1e3230) has its Draw first;
    // a View 7 an Anim 0 before both.
    struct Parts
    {
        Trans trans;
        Draw draw;
        Anim anim; // a View's
        size_t rest = 0u;
    };

    std::optional<Parts> parts(const std::string &cls, const Bytes &b);

    // An unbuilt Mesh 25's strips (RndMesh::Load, 0x3d5420, reads them
    // of any 25): for each of its face groups a strip a face, as the
    // strip count, the vert total, each strip's end but the last, and
    // the verts. `o` is past its Draw: its mat, its geometry's owner,
    // nine bytes, its verts of 48 bytes, its faces of three shorts, its
    // groups' sizes and its bones.
    Bytes strips(const Bytes &b, size_t o);

    // The strings in a body that end `suffix`.
    std::vector<std::string> named(const Bytes &b, const std::string &suffix);

    // Whether a body holds `name` as a string.
    bool holds(const Bytes &b, const std::string &name);

    std::optional<milo::Dir> loadScene(size_t disc, const std::string &path);

    // A Tex 8 around that bitmap (RndTex::Load, GH1 0x1e2260): its size,
    // bits a pixel and path, nine bytes, then a bitmap in a built scene,
    // which GH2 reads after a last byte of 0. A bitmap's own header
    // (RndBitmap::LoadHeader, 0x1adeb8) has its bits a pixel at 1, its
    // width at 7 and its height at 9: `sized` gives the Tex those. Nothing
    // if either is cut short.
    Bytes texWith(const Bytes &tex, const Bytes &bitmap, bool sized);
}
