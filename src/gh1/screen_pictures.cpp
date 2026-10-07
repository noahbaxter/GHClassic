// GH1's pictures as meshes.
//
// A PictureEx 1 (RndPictureEx::Load, GH1 0x122910) is a Trans 8, a Draw 1
// and a style, which names a Mesh of ghui/resources.rnd for its picture and,
// for some, another for when it is disabled (ghui/config.dta's pictureex).
// GH2 makes nothing of one (DirLoader::FixClassName, 0x2be930, names it
// BandPicture, which has no factory), so each is that Mesh here, with its
// Mat and Tex, where the picture stands, and the disabled one beside it
// unshown, as <name>_lock.<ext>, for a script to show.

#include "gh1/screen.h"

namespace gh2::gh1
{
    namespace
    {
        using milo::putStr;
        using milo::putU32;
        using milo::str;
    }

    std::string Conversion::lockOf(const std::string &picture)
    {
        const size_t dot = picture.rfind('.');
        return picture.substr(0, dot) + "_lock" + (dot == std::string::npos ? "" : picture.substr(dot));
    }

    // That picture's style, and its disabled Mesh's place in the tree.
    void Conversion::readPicture(const std::string &n, const Bytes &b)
    {
        size_t o = part[n].rest;
        pictures[n] = str(b, o);
        const dtb::Node *style = look.style("pictureex", pictures[n]);
        if (!style || !dtb::find(*style, "disabled_mesh"))
            return;
        const std::string lock = lockOf(n);
        locks[n] = lock;
        part[lock] = part[n];
        cls[lock] = "PictureEx";
    }

    // That object of ghui/resources.rnd, with what it draws with.
    void Conversion::addResource(const std::string &cls, const std::string &name)
    {
        const milo::Dir &from = look.resources;
        const auto i = milo::find(from, name);
        if (milo::find(out, name) || !i || from.entries[*i].first != cls)
            return;
        if (cls == "Mat")
            for (const std::string &tex : named(from.bodies[*i], ".tex"))
                addResource("Tex", tex);
        Bytes body = from.bodies[*i];
        // A Mesh here lends its verts and is not itself drawn: its Draw 1's
        // showing flag follows its Trans 8.
        if (const auto of = cls == "Mesh" ? parts(cls, body) : std::nullopt)
            body[of->trans.end + 4u] = 0u;
        milo::add(out, cls, name, std::move(body));
    }

    // The Mesh that style names under `key`, as `name` at the picture's
    // place: its own geometry (a Mesh 25's mat, then the Mesh that owns its
    // verts, itself or another of the resources) under the picture's Trans
    // and Draw.
    bool Conversion::addPictureMesh(const std::string &name, const std::string &picture, const Bytes &b,
                                    const char *key, bool showing)
    {
        const Parts &p = part[picture];
        const dtb::Node *style = look.style("pictureex", pictures[picture]);
        const dtb::Node *mesh = style ? dtb::find(*style, key) : nullptr;
        const milo::Dir &from = look.resources;
        const std::string theirName = mesh && mesh->nodes.size() > 1u ? mesh->nodes[1].text : std::string();
        const auto i = milo::find(from, theirName);
        const auto of = i && from.entries[*i].first == "Mesh" ? parts("Mesh", from.bodies[*i]) : std::nullopt;
        if (!of)
            return false;
        const Bytes &theirs = from.bodies[*i];
        Bytes body = revision(theirs);
        putU32(body, 8u);
        // A picture's Mesh lies in x and y, and stands up in the picture's
        // x and z: its y axis is the picture's z, and its z the picture's y
        // turned back.
        for (const size_t xfm : {p.trans.at + 4u, p.trans.at + 52u})
        {
            put(body, b, xfm, xfm + 12u);
            put(body, b, xfm + 24u, xfm + 36u);
            for (size_t f = 0; f < 3u; ++f)
                putF32(body, -f32(b, xfm + 12u + 4u * f));
            put(body, b, xfm + 36u, xfm + 48u);
        }
        putNames(body, {});
        put(body, b, p.trans.constraint, p.trans.parent);
        putStr(body, name);
        putU32(body, 1u);
        body.push_back(showing ? 1u : 0u);
        putNames(body, {});
        put(body, b, p.draw.sphere, p.draw.sphere + 16u);
        size_t o = of->rest;
        const std::string mat = str(theirs, o);
        const std::string owner = str(theirs, o);
        putStr(body, mat);
        putStr(body, owner == theirName ? name : owner);
        put(body, theirs, o, theirs.size());
        addResource("Mat", mat);
        if (owner != theirName)
            addResource("Mesh", owner);
        milo::add(out, "Mesh", name, std::move(body));
        return true;
    }

    bool Conversion::addPicture(const std::string &n, const Bytes &b)
    {
        if (!addPictureMesh(n, n, b, "mesh", shows(n, part[n])))
            return false;
        const auto lock = locks.find(n);
        return lock == locks.end() || addPictureMesh(lock->second, n, b, "disabled_mesh", false);
    }
}
