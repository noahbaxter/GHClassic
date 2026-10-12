// Each of a GH1 scene's objects that is no widget as GH2's.

#include "gh1/screen.h"

#include "disc/ark.h"

#include <cstring>

namespace gh2::gh1
{
    namespace
    {
        using milo::putStr;
        using milo::putU32;
        using milo::str;
        using milo::u32;
    }

    // Its Trans 8 with the children that are here:
    // RndTransformable::Load (0x3d72d0) parents each without a check.
    void Conversion::putTrans(Bytes &out, const Bytes &b, const Parts &p)
    {
        put(out, b, p.trans.at, p.trans.at + 100u);
        std::vector<std::string> children;
        for (const std::string &child : p.trans.children)
            if (part.count(child))
            {
                children.push_back(child);
                if (const auto lock = locks.find(child); lock != locks.end())
                    children.push_back(lock->second);
            }
        putNames(out, children);
        put(out, b, p.trans.constraint, p.trans.end);
    }

    // Its Draw 1 with those children.
    void Conversion::putDraw(Bytes &out, const Bytes &b, const std::string &n, const Parts &p,
                             const std::vector<std::string> &children)
    {
        putU32(out, 1u);
        out.push_back(shows(n, p) ? 1u : 0u);
        putNames(out, children);
        put(out, b, p.draw.sphere, p.draw.sphere + 16u);
    }

    // These have no bitmap: GH1 loads the path's file,
    // <dir>/gen/<name>_ps2 (Rnd::CacheResource, GH1 0x1de058).
    bool Conversion::addTex(const std::string &n, const Bytes &b)
    {
        size_t o = 16u;
        const std::string path = str(b, o);
        Bytes body = b;
        if (b.size() == o + 9u)
        {
            Bytes bitmap;
            if (!path.empty())
            {
                const size_t slash = path.rfind('/');
                const std::string dir = slash == std::string::npos ? std::string() : path.substr(0, slash + 1u);
                auto file = ark::readFile(disc, kFolder + dir + "gen/" + path.substr(dir.size()) + "_ps2");
                if (!file)
                    return false;
                bitmap = std::move(*file);
            }
            else
            {
                // One a script fills: a clear bitmap of its size
                // (RndBitmap::LoadHeader, 0x1adeb8: a revision, bits a
                // pixel, an order, mips, width, height and row bytes in 32
                // bytes, then a palette).
                const uint32_t width = u32(b, 4u), height = u32(b, 8u);
                if (width > 1024u || height > 1024u)
                    return false;
                bitmap.assign(32u + 1024u + static_cast<size_t>(width) * height, 0u);
                bitmap[0] = 1u;
                bitmap[1] = 8u;
                bitmap[2] = 3u;
                for (size_t at : {7u, 11u})
                    std::memcpy(bitmap.data() + at, &width, 2u);
                std::memcpy(bitmap.data() + 9u, &height, 2u);
            }
            body = texWith(b, bitmap, false);
            if (body.empty())
                return false;
        }
        milo::add(out, "Tex", n, std::move(body));
        return true;
    }

    void Conversion::addAnim(const std::string &c, const std::string &n, const Bytes &b)
    {
        Bytes body = revision(b);
        const auto &[own, rest] = anims[n];
        if (own.filtered())
            milo::add(out, "AnimFilter", filterOf(n), filter(n, own));
        putAnim(body);
        if (c != "MatAnim")
        {
            put(body, b, rest, b.size());
            milo::add(out, c, n, std::move(body));
            return;
        }
        // Its first stage alone, or none with no material.
        const Stages s = stages(b, rest);
        size_t o = rest;
        const std::string mat = str(b, o);
        putStr(body, mat);
        const bool first = !mat.empty() && !s.keyed.empty();
        putU32(body, first ? 1u : 0u);
        if (first)
            put(body, b, s.at[0], s.at[1]);
        put(body, b, s.at.back(), b.size());
        milo::add(out, c, n, std::move(body));
        for (size_t k = 1u; k < s.keyed.size(); ++k)
        {
            if (!s.keyed[k])
                continue;
            const std::string stage = stageOf(n, k);
            Bytes made = revision(b);
            putAnim(made);
            putStr(made, base(mat) + "_" + std::to_string(k) + ".mat");
            putU32(made, 1u);
            put(made, b, s.at[k], s.at[k + 1u]);
            putStr(made, {});
            putU32(made, 0u);
            putU32(made, 0u);
            milo::add(out, c, stage, std::move(made));
            if (own.filtered())
                milo::add(out, "AnimFilter", filterOf(stage), filter(stage, own));
        }
    }

    // One of GH1's widgets that GH2 loads as it is, as GH2's class `as`.
    void Conversion::addKept(const std::string &c, const std::string &as, const std::string &n, const Bytes &b)
    {
        Bytes body = revision(b);
        const Parts &p = part[n];
        if (c == "Placer")
        {
            putDraw(body, b, n, p, {});
            putTrans(body, b, p);
        }
        else
        {
            putTrans(body, b, p);
            putDraw(body, b, n, p, {});
        }
        put(body, b, p.rest, b.size());
        milo::add(out, as, n, std::move(body));
    }

    void Conversion::addMesh(const std::string &n, const Bytes &b)
    {
        Bytes body = revision(b);
        const Parts &p = part[n];
        putTrans(body, b, p);
        putDraw(body, b, n, p, {});
        put(body, b, p.rest, b.size());
        const Bytes made = strips(b, p.rest);
        body.insert(body.end(), made.begin(), made.end());
        milo::add(out, "Mesh", n, std::move(body));
    }

    // A View 7 is a Group to GH2: RndGroup::Load (0x3d5330) takes
    // revisions under 8. Its Anim 0 drives nothing: its list does.
    void Conversion::addView(const std::string &n, const Bytes &b)
    {
        Bytes body = revision(b);
        const Parts &p = part[n];
        putU32(body, 0u);
        putU32(body, 0u);
        putU32(body, 0u);
        putTrans(body, b, p);
        putDraw(body, b, n, p, lists[n]);
        milo::add(out, "Group", n, std::move(body));
        if (wrapped.count(n))
            milo::add(out, "AnimFilter", filterOf(n), filter(n, p.anim));
    }

    void Conversion::addText(const std::string &n, const Bytes &b)
    {
        Bytes body = revision(b);
        const Parts &p = part[n];
        putDraw(body, b, n, p, {});
        putTrans(body, b, p);
        put(body, b, p.rest, b.size());
        milo::add(out, "Text", n, std::move(body));
    }
}
