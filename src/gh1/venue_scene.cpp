// A GH1 scene as GH2 draws it.
//
// GH1 draws a scene from its top View down: each View its Draw 1's
// children in order, each Mesh itself and then its own. GH2's dir
// draws every drawable no Group lists (RndDir::SyncObjects,
// 0x1b2f78), so what GH1's tree leaves out is hidden here.

#include "gh1/venue_scene.h"

#include "gh1/scene.h"

#include <algorithm>
#include <map>
#include <optional>

namespace gh2::gh1
{
    namespace
    {
        using milo::putStr;
        using milo::putU32;
        using milo::str;
        using milo::u32;

        const std::set<std::string> kAnims = {"TransAnim", "MatAnim", "LightAnim", "EnvAnim", "MeshAnim", "ParticleSysAnim"};

        // A scene's objects as read, those in the classes asked for.
        struct Scan
        {
            const milo::Dir &scene;
            // The anims a script drives itself.
            const std::set<std::string> &scripted;
            struct Piece
            {
                std::optional<Anim> anim;
                size_t rest = 4u; // past its Anim 0
            };
            std::map<std::string, size_t> entry; // by name, its index
            std::map<std::string, Piece> pieces;
            std::map<std::string, Trans> trans;
            std::map<std::string, std::vector<std::string>> draws; // by Mesh, View or Environ, its Draw 1's children
            std::set<std::string> meshes, environs;
            std::set<std::string> moved; // what a TransAnim moves
            std::set<std::string> hung;  // what the top View's Trans reach
        };

        Scan scan(const milo::Dir &scene, const std::set<std::string> &classes, const std::set<std::string> &without,
                  const std::set<std::string> &scripted)
        {
            Scan s{scene, scripted};
            std::map<std::string, std::string> owners; // by View, its children's owner
            for (size_t i = 0; i < scene.entries.size(); ++i)
            {
                const auto &[c, n] = scene.entries[i];
                const Bytes &b = scene.bodies[i];
                s.entry[n] = i;
                if (!classes.count(c) || without.count(n))
                    continue;
                if (c == "Environ" && u32(b, 0u) == 1u && u32(b, 4u) == 1u)
                {
                    size_t o = 9u;
                    s.draws[n] = names(b, o);
                    s.environs.insert(n);
                }
                Scan::Piece p;
                if (c == "View" || c == "ParticleSys" || kAnims.count(c))
                {
                    p.anim = anim(b, p.rest);
                    if (!p.anim)
                        continue;
                    // A TransAnim 4 (RndTransAnim::Load, GH1 0x1dd538): after
                    // its Anim 0 a Draw 1, then the Trans it moves.
                    if (c == "TransAnim" && u32(b, p.rest) == 1u)
                    {
                        size_t o = p.rest + 5u;
                        names(b, o);
                        o += 16u;
                        s.moved.insert(str(b, o));
                    }
                }
                else if (c != "Mesh" && c != "Light" && c != "Flare")
                    continue;
                if (!kAnims.count(c))
                {
                    size_t o = p.rest;
                    const auto t = trans(b, o);
                    if (!t)
                        continue;
                    s.trans[n] = *t;
                    if (c == "Mesh" || c == "View")
                    {
                        o += 5u;
                        s.draws[n] = names(b, o);
                        // Past a View's sphere, the View whose children it has
                        // (RndView::Load, GH1 0x2eb3f0).
                        o += 16u;
                        if (c == "View" && u32(b, 0u) > 3u)
                            owners[n] = str(b, o);
                    }
                    if (c == "Mesh")
                        s.meshes.insert(n);
                }
                s.pieces[n] = std::move(p);
            }

            // A View with another's children draws those and then its own
            // (RndView::DrawShowing, GH1 0x1ef8f0).
            for (const auto own = s.draws; const auto &[view, owner] : owners)
                if (const auto it = own.find(owner); owner != view && it != own.end())
                    s.draws[view].insert(s.draws[view].begin(), it->second.begin(), it->second.end());
            return s;
        }

        // What a Mesh, View or Environ draws after itself.
        const std::vector<std::string> &after(const Scan &s, const std::string &n)
        {
            static const std::vector<std::string> kNone;
            const auto it = s.draws.find(n);
            return it != s.draws.end() ? it->second : kNone;
        }

        // GH1 works a world out only from a section's View down, each
        // Trans its children's (VenueSection::Poll, GH1 0x178ce8;
        // RndTransformable::UpdateWorldXfm, GH1 0x1da570): what that
        // never reaches keeps the world it loads with, here its local
        // with no parent. One a TransAnim moves is left as it is.
        void hang(Scan &s, const std::string &n)
        {
            const auto it = s.trans.find(n);
            if (it == s.trans.end() || !s.hung.insert(n).second)
                return;
            for (const std::string &child : it->second.children)
                hang(s, child);
        }

        bool held(const Scan &s, const std::string &n)
        {
            return !s.hung.count(n) && !s.moved.count(n);
        }

        // The anims an anim drives that are here, less those a script drives.
        std::vector<std::string> driven(const Scan &s, const Anim &of)
        {
            std::vector<std::string> out;
            for (const std::string &child : of.children)
                if (const auto it = s.pieces.find(child); it != s.pieces.end() && it->second.anim && !s.scripted.count(child))
                    out.push_back(child);
            return out;
        }

        // What stands for each anim: a View's anims if it has any here,
        // a MatAnim's later stages after it.
        void stand(const Scan &s, Drivers &drivers, const std::string &n, int depth)
        {
            const auto it = s.pieces.find(n);
            if (it == s.pieces.end() || !it->second.anim || drivers.count(n))
                return;
            const Anim &a = *it->second.anim;
            const size_t i = s.entry.at(n);
            const std::string &c = s.scene.entries[i].first;
            std::vector<std::string> as;
            if (c == "View")
            {
                bool any = false;
                for (const std::string &child : driven(s, a))
                {
                    if (depth < 16)
                        stand(s, drivers, child, depth + 1);
                    any |= drivers.count(child) && !drivers[child].empty();
                }
                if (any)
                    as.push_back(filterOf(n));
            }
            else
            {
                // A ParticleSys is drawn by whatever Group it is in, so
                // its filter stands for it filtered or not.
                as.push_back(a.filtered() || c == "ParticleSys" ? filterOf(n) : n);
                const Stages st = c == "MatAnim" ? stages(s.scene.bodies[i], it->second.rest) : Stages();
                for (size_t k = 1u; k < st.keyed.size(); ++k)
                    if (st.keyed[k])
                        as.push_back(a.filtered() ? filterOf(stageOf(n, k)) : stageOf(n, k));
            }
            drivers[n] = std::move(as);
        }

        // A View 7 of those anims and draws: its Anim 0, a Trans 8 at the
        // origin under itself, and a Draw 1 showing, as a Group must be to
        // set a frame (RndGroup::SetFrame, 0x1ba8a0).
        Bytes group(const std::string &name, const std::vector<std::string> &anims,
                    const std::vector<std::string> &draws)
        {
            Bytes out;
            putU32(out, 7u);
            putU32(out, 0u);
            putU32(out, 0u);
            putNames(out, anims);
            putU32(out, 8u);
            for (int i = 0; i < 2; ++i)
                for (int f = 0; f < 12; ++f)
                    putF32(out, f == 0 || f == 4 || f == 8 ? 1.0f : 0.0f);
            putU32(out, 0u);
            putU32(out, 0u);
            putStr(out, {});
            out.push_back(0u);
            putStr(out, name);
            putU32(out, 1u);
            out.push_back(1u);
            putNames(out, draws);
            out.insert(out.end(), 16u, 0u);
            return out;
        }

        std::string animsOf(const std::string &view)
        {
            return view + ".anims";
        }

        // GH1 sets a scene's frame from its top View down: each anim takes
        // its View's frame through its own filters and hands it to its
        // children (RndAnimatable::SetFrame, GH1 0x1b3d00). GH2's
        // RndAnimatable::Load (0x1ab228) takes an Anim 0's filters out into
        // an AnimFilter that nothing drives, and a Group sets the frame of
        // every anim in it, those it draws too. So an anim's filters are an
        // AnimFilter made here, and a View's anims a Group of their own
        // beside the one it draws, "<view>.anims", unshown: what stands for
        // an anim, to whatever drives it, is its filter or else itself.
        void animated(const Scan &s, Drivers &drivers, const std::string &c, const std::string &n, const Anim &a,
                      Bytes &body, std::vector<Object> &out)
        {
            if (c != "View")
            {
                putAnim(body);
                if (a.filtered() || c == "ParticleSys")
                    out.push_back({"AnimFilter", filterOf(n), filter(n, a)});
                return;
            }
            // A View draws; its anims are their own Group's.
            putU32(body, 0u);
            putU32(body, 0u);
            putU32(body, 0u);
            std::vector<std::string> anims;
            for (const std::string &child : driven(s, a))
                anims.insert(anims.end(), drivers[child].begin(), drivers[child].end());
            if (!anims.empty())
            {
                out.push_back({"Group", animsOf(n), group(animsOf(n), anims, {})});
                out.push_back({"AnimFilter", filterOf(n), filter(animsOf(n), a)});
            }
        }

        // RndMatAnim::LoadStages (0x1c2c28) hands a stage after the first to
        // an anim nothing drives, of the pass RndMat::LoadStages (0x1bf350)
        // made of that stage, or loads it over the first. So a MatAnim is
        // its first stage alone, or none with no material, and each later
        // one that has keys an anim of its own, "<anim>_<n>.mnm" of
        // "<mat>_<n>.mat", driven with the first.
        void matAnim(const std::string &n, const Bytes &b, size_t rest, const Anim &a, Bytes body,
                     std::vector<Object> &out)
        {
            const Stages s = stages(b, rest);
            size_t o = rest;
            const std::string mat = str(b, o);
            putStr(body, mat);
            const bool first = !mat.empty() && !s.keyed.empty();
            putU32(body, first ? 1u : 0u);
            if (first)
                put(body, b, s.at[0], s.at[1]);
            put(body, b, s.at.back(), b.size());
            out.push_back({"MatAnim", n, std::move(body)});
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
                out.push_back({"MatAnim", stage, std::move(made)});
                if (a.filtered())
                    out.push_back({"AnimFilter", filterOf(stage), filter(stage, a)});
            }
        }

        // A Trans 8 with the world for both its local and world where it is
        // held. RndTransformable::Load (0x3d72d0) parents each of its
        // children without a check that it is there, so the list keeps only
        // those that are. Its constraint, target and preserve scale stay;
        // its parent is the object's own name for none, as here when the
        // one it names is left out.
        void placed(const Scan &s, const std::string &n, const Bytes &b, const Trans &t, Bytes &body)
        {
            const size_t local = t.at + 4u, world = t.at + 52u, children = t.at + 100u;
            const bool still = held(s, n);
            put(body, b, t.at, local);
            put(body, b, still ? world : local, still ? children : world);
            put(body, b, world, children);
            std::vector<std::string> kept = t.children;
            std::erase_if(kept, [&](const std::string &child) { return s.trans.count(child) == 0u || held(s, child); });
            putNames(body, kept);
            put(body, b, t.constraint, t.parent);
            size_t o = t.parent;
            const std::string named = str(b, o);
            putStr(body, s.trans.count(named) ? named : n);
        }

        // The View a section draws after the band, by the one it draws
        // before.
        std::string transparentOf(const std::string &view)
        {
            return base(view) + "_transparent.view";
        }

        // Whether that Environ lights the band, the guitarists or the crowd.
        bool ofCharacters(const std::string &n)
        {
            return n == "stagechar.env" || n == "crowd.env" ||
                   (n.size() == 11u && n.rfind("singer", 0) == 0 && n.substr(7u) == ".env");
        }

        struct Listing
        {
            std::vector<std::string> own;
            std::vector<std::vector<std::string>> runs; // each its Environ first
            std::string exit;                           // the Environ it leaves in use, if it sets one
        };

        // GH1's View tree as GH2's Groups.
        //
        // A View 7 is a Group to GH2: RndGroup::Load (0x3d5330) takes
        // revisions under 8, and RndDrawable::Load (0x3d5090) adds a Draw
        // 1's children to the group it is loading (an Environ among them as
        // the group's own, a Cam not at all). It drops a Mesh's, so each
        // View lists them here after their Mesh.
        //
        // A View's list: its children, each Mesh's and Environ's own
        // after it. GH1 draws a drawable and then its children
        // (RndDrawable::Draw, GH1 0x2ec850), and an Environ drawn is the
        // one in use from then on, past its View's end
        // (RndEnviron::DrawShowing, GH1 0x2b69e0). A Group puts back the
        // one before it (RndGroup::DrawShowing, 0x1bab10), so what a View
        // draws from an Environ on, or after a View that left another in
        // use, is a Group of that Environ's, "<view>.env<n>".
        //
        // A section's "<name>_transparent.view" is taken out of the View
        // that lists it (VenueSection::IsLoaded, GH1 0x178db8) and drawn
        // after the band (ArenaPanel::Draw, GH1 0x10d488). The Environs
        // of the band, the guitarists and the crowd are taken out too
        // (Arena::SetupEnvs, GH1 0x168370).
        struct Tree
        {
            const Scan &s;
            const std::set<std::string> &hidden;
            std::string apart;
            std::map<std::string, Listing> listings;
            // An Environ's own children, a Group "<environ>.draws" first in
            // its run: unshown, GH1's Environ draws none of them and is not
            // put in use.
            std::map<std::string, std::vector<std::string>> owned;
            // What the tree draws: a Mesh or View reached, and what each lists.
            std::set<std::string> drawn;

            const Listing &listed(const std::string &view, int depth);
            void add(Listing &l, int depth, const std::string &n, int within, std::vector<std::string> *into);
            void reach(const std::string &n);
        };

        const Listing &Tree::listed(const std::string &view, int depth)
        {
            if (const auto it = listings.find(view); it != listings.end())
                return it->second;
            listings[view];
            Listing l;
            for (const std::string &child : after(s, view))
                add(l, depth, child, 0, nullptr);
            return listings[view] = std::move(l);
        }

        void Tree::add(Listing &l, int depth, const std::string &n, int within, std::vector<std::string> *into)
        {
            if (n == apart || ofCharacters(n))
                return;
            if (s.environs.count(n) && !into)
            {
                l.runs.push_back({n});
                l.exit = n;
                if (!after(s, n).empty() && !owned.count(n))
                {
                    std::vector<std::string> &mine = owned[n];
                    for (const std::string &child : after(s, n))
                        add(l, depth, child, within + 1, &mine);
                    l.runs.back().push_back(drawsOf(n));
                }
                return;
            }
            (into ? *into : l.runs.empty() ? l.own : l.runs.back()).push_back(n);
            if (s.meshes.count(n) && within < 8)
                for (const std::string &child : after(s, n))
                    add(l, depth, child, within + 1, into);
            else if (!into && s.draws.count(n) && !s.meshes.count(n) && !s.environs.count(n) && depth < 16)
                if (const std::string left = listed(n, depth + 1).exit; !left.empty() && left != l.exit)
                {
                    l.runs.push_back({left});
                    l.exit = left;
                }
        }

        void Tree::reach(const std::string &n)
        {
            if (hidden.count(n) || !drawn.insert(n).second)
                return;
            for (const std::string &child : after(s, n))
                reach(child);
        }

        // Its Draw 1: a revision, the showing flag, the children, a sphere.
        // A View's children are its list, each run a Group.
        void drawing(Tree &tree, const std::string &c, const std::string &n, const Bytes &b, size_t draw, Bytes &body,
                     std::vector<Object> &out)
        {
            put(body, b, draw, draw + 4u);
            body.push_back(tree.drawn.count(n) ? u8(b, draw + 4u) : 0u);
            if (c != "View")
                put(body, b, draw + 5u, b.size());
            else
            {
                const Listing &l = tree.listed(n, 0);
                std::vector<std::string> list = l.own;
                for (size_t k = 0; k < l.runs.size(); ++k)
                {
                    list.push_back(n + ".env" + std::to_string(k + 1u));
                    out.push_back({"Group", list.back(), group(list.back(), {}, l.runs[k])});
                }
                putNames(body, list);
                size_t o = draw + 5u;
                names(b, o);
                put(body, b, o, o + 16u);
            }
            out.push_back({c == "View" ? "Group" : c, n, std::move(body), c == "Mesh" && !tree.drawn.count(n)});
        }

        // An Environ 1 less the children its Draw 1 lists.
        Bytes withoutDraws(const Bytes &b)
        {
            size_t o = 9u;
            names(b, o);
            Bytes out(b.begin(), b.begin() + 9);
            putU32(out, 0u);
            put(out, b, o, b.size());
            return out;
        }
    }

    std::vector<Object> objects(const milo::Dir &scene, const std::set<std::string> &classes, const std::string &top,
                                const std::set<std::string> &without, const std::set<std::string> &hidden,
                                const std::set<std::string> &scripted, Drivers &drivers)
    {
        Scan s = scan(scene, classes, without, scripted);
        hang(s, top);
        for (const auto &e : scene.entries)
            stand(s, drivers, e.second, 0);
        Tree tree{s, hidden, transparentOf(top)};
        tree.reach(top);
        tree.reach(tree.apart);

        std::vector<Object> out;
        for (size_t i = 0; i < scene.entries.size(); ++i)
        {
            const auto &[c, n] = scene.entries[i];
            const Bytes &b = scene.bodies[i];
            if (!classes.count(c) || without.count(n))
                continue;
            const auto piece = s.pieces.find(n);
            if (piece == s.pieces.end())
            {
                if (c == "Tex" || c == "Mat")
                    out.push_back({c, n, b});
                else if (c == "Environ")
                    out.push_back({c, n, s.environs.count(n) ? withoutDraws(b) : b});
                continue;
            }
            const Scan::Piece &p = piece->second;
            Bytes body = revision(b);
            if (p.anim)
                animated(s, drivers, c, n, *p.anim, body, out);
            if (c == "MatAnim")
            {
                matAnim(n, b, p.rest, *p.anim, std::move(body), out);
                continue;
            }
            const auto t = s.trans.find(n);
            if (t == s.trans.end())
            {
                put(body, b, p.rest, b.size());
                out.push_back({c, n, std::move(body)});
                continue;
            }
            placed(s, n, b, t->second, body);
            if (c == "Light")
            {
                put(body, b, t->second.end, b.size());
                out.push_back({c, n, std::move(body)});
                continue;
            }
            drawing(tree, c, n, b, t->second.end, body, out);
        }
        for (const auto &[of, list] : tree.owned)
            out.push_back({"Group", drawsOf(of), group(drawsOf(of), {}, list)});
        return out;
    }
}
