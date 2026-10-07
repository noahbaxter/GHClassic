#include "gh1/screen.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace gh2::gh1
{
    namespace
    {
        using Row = Screen::Row;

        const std::set<std::string> kOut = {"Cam", "Environ", "Light"};
        // GH1's widgets GH2 still loads as they are, under its own
        // names for them (DirLoader::FixClassName, 0x2be930, renames
        // them in files older than this one): BandPlacer::Load
        // (0x290f28), BandSlider::PreLoad (0x296908) and
        // BandTextEntry::PreLoad (0x2984f8) keep their revisions.
        const std::map<std::string, std::string> kKept = {
            {"Placer", "BandPlacer"}, {"Slider", "BandSlider"}, {"TextEntry", "BandTextEntry"}};
        const std::set<std::string> kAnims = {"MatAnim", "TransAnim", "EnvAnim"};
    }

    std::optional<milo::Dir> Conversion::run()
    {
        if (!tree())
            return std::nullopt;
        addRows();
        fill();
        reach(top);
        for (const Row &row : screen.rows)
            if (row.of && drawn.count(row.of))
                drawn.insert(row.name);
        if (!readAnims())
            return std::nullopt;
        for (const auto &[n, c] : cls)
            if (c == "View" && part.count(n))
                lists[n] = listed(n);
        drawRest();
        out = gh2;
        out.entries.clear();
        out.bodies.clear();
        for (size_t i = 0; i < gh1.entries.size(); ++i)
            if (!object(i))
                return std::nullopt;
        takeFromGh2();
        return std::move(out);
    }

    // Each object's class and parts, and each child's parent.
    bool Conversion::tree()
    {
        for (size_t i = 0; i < gh1.entries.size(); ++i)
        {
            const auto &[c, n] = gh1.entries[i];
            cls[n] = c;
            if (c == "Mesh" || c == "View" || c == "Text" || c == "LabelEx" || c == "ButtonEx" || c == "PictureEx" ||
                kKept.count(c))
            {
                auto p = parts(c, gh1.bodies[i]);
                if (!p)
                    return false;
                part[n] = std::move(*p);
                if (c == "PictureEx")
                    readPicture(n, gh1.bodies[i]);
            }
        }
        for (const auto &[n, p] : part)
            for (const std::string &child : p.trans.children)
                if (child != n)
                    parent[child] = n;
        // A Mesh that stands where a View's world puts its local is under
        // that View too: GH1 keeps some so without listing them
        // (store_back.gh's us_insidewalls.mesh under us_inside.view), and
        // GH2 works a world out from the parents it is told. A Trans is a
        // revision, a local and a world, each three rows and a position.
        // The same to a twentieth of a unit and a thousandth part, which
        // GH1's own worlds are within.
        const auto matrix = [&](const std::string &n, size_t o)
        {
            std::array<float, 12> m{};
            for (size_t f = 0; f < 12u; ++f)
                m[f] = f32(gh1.bodies[*milo::find(gh1, n)], part[n].trans.at + o + 4u * f);
            return m;
        };
        for (auto &[view, p] : part)
        {
            if (cls[view] != "View")
                continue;
            const auto over = matrix(view, 52u);
            for (const auto &[c, child] : gh1.entries)
            {
                if (c != "Mesh" || parent.count(child) || !part.count(child))
                    continue;
                const auto local = matrix(child, 4u), world = matrix(child, 52u);
                bool under = true, moved = false;
                for (size_t row = 0; row < 4u; ++row)
                    for (size_t axis = 0; axis < 3u; ++axis)
                    {
                        float there = row == 3u ? over[9u + axis] : 0.0f;
                        for (size_t k = 0; k < 3u; ++k)
                            there += local[3u * row + k] * over[3u * k + axis];
                        const float is = world[3u * row + axis];
                        under = under && std::fabs(there - is) <= 0.05f + 0.001f * std::fabs(is);
                        moved = moved || std::fabs(local[3u * row + axis] - is) > 0.05f;
                    }
                if (under && moved)
                {
                    parent[child] = view;
                    p.trans.children.push_back(child);
                }
            }
        }
        return true;
    }

    // The screen's rows, each linked to the next, and each with `of` as a ButtonEx like it.
    void Conversion::addRows()
    {
        for (size_t i = 0; i < screen.rows.size(); ++i)
        {
            const Row &row = screen.rows[i];
            rows[row.name] = &row;
            linked[row.name] = screen.rows[(i + 1u) % screen.rows.size()].name;
            if (row.of && part.count(row.of))
            {
                part[row.name] = part[row.of];
                cls[row.name] = "ButtonEx";
                if (const auto owner = parent.find(row.of); owner != parent.end())
                    parent[row.name] = owner->second;
            }
        }
    }

    // What GH2's scene gives in place of what GH1's has no like of
    // here: its widgets, filters and triggers, and anything else of
    // its own that the scripts name or the screen places.
    void Conversion::fill()
    {
        static const std::set<std::string> kArt = {"Tex", "Mat", "Mesh", "Group", "Text", "Font", "Cam", "Environ", "Light"};
        static const std::set<std::string> kMade = {"Tex", "Mat", "Mesh", "View", "Text", "LabelEx", "ButtonEx", "PictureEx"};
        for (const auto &[c, n] : gh2.entries)
            if (const auto own = cls.find(n);
                (!kArt.count(c) || scripted.count(n)) &&
                (own == cls.end() || !(kMade.count(own->second) || kKept.count(own->second) || kAnims.count(own->second))))
                filled.insert(n);
        for (const char *n : screen.without)
            filled.erase(n);
        for (const auto &[n, to] : screen.frames)
            filled.erase(n);
        for (const char *n : screen.with)
        {
            kept.insert(n);
            if (!cls.count(n))
                filled.insert(n);
        }
        for (const Screen::Place &place : screen.placed)
            if (!cls.count(place.name))
                filled.insert(place.name);
    }

    // GH1 draws a scene from its top View down: each View its Draw
    // 1's children in order, each Mesh itself and then its own.
    // GH2's dir draws every drawable no Group lists
    // (RndDir::SyncObjects, 0x1b2f78), so what the tree leaves out
    // is unshown here.
    void Conversion::reach(const std::string &n)
    {
        const auto it = part.find(n);
        if (it == part.end() || !drawn.insert(n).second)
            return;
        for (const std::string &child : it->second.draw.children)
            reach(child);
    }

    bool Conversion::readAnims()
    {
        for (size_t i = 0; i < gh1.entries.size(); ++i)
            if (kAnims.count(gh1.entries[i].first))
            {
                size_t o = 4u;
                auto a = anim(gh1.bodies[i], o);
                if (!a)
                    return false;
                anims[gh1.entries[i].second] = {std::move(*a), o};
            }
        return true;
    }

    // A View sets the frame of the anims it names, each through that
    // anim's own filters (RndAnimatable::SetFrame, GH1 0x1b3d00). A
    // Group sets the frame of every anim it lists, those it draws
    // too (RndGroup::SetFrame, 0x1ba8a0), and GH2 takes an Anim 0's
    // filters out into an AnimFilter nothing drives (0x1ab228). So
    // an anim with filters is listed as an AnimFilter made here, and
    // so is a View its parent drives and does not draw.
    // RndMatAnim::LoadStages (0x1c2c28) loads a stage after the
    // first over the first, so each with keys is an anim of its
    // own, "<anim>_<n>.mnm" of the pass RndMat::LoadStages
    // (0x1bf350) makes of that stage, "<mat>_<n>.mat".
    std::vector<std::string> Conversion::stands(const std::string &view, const std::string &a)
    {
        std::vector<std::string> out;
        if (const auto own = anims.find(a); own != anims.end())
        {
            const bool filtered = own->second.first.filtered();
            out.push_back(filtered ? filterOf(a) : a);
            if (const auto i = milo::find(gh1, a); i && cls[a] == "MatAnim")
            {
                const Stages s = stages(gh1.bodies[*i], own->second.second);
                for (size_t k = 1u; k < s.keyed.size(); ++k)
                    if (s.keyed[k])
                        out.push_back(filtered ? filterOf(stageOf(a, k)) : stageOf(a, k));
            }
        }
        else if (const auto sub = part.find(a); sub != part.end() && cls[a] == "View" && a != view)
        {
            const std::vector<std::string> &shown = part[view].draw.children;
            const bool here = std::find(shown.begin(), shown.end(), a) != shown.end();
            if (sub->second.anim.filtered() || !here)
            {
                wrapped.insert(a);
                out.push_back(filterOf(a));
            }
        }
        return out;
    }

    // `n` in a Group's list, and what follows it there.
    void Conversion::follow(std::vector<std::string> &out, const std::string &n, int depth)
    {
        const auto own = part.find(n);
        if (own == part.end() && !filled.count(n))
            return;
        out.push_back(n);
        for (const Row &row : screen.rows)
            if (row.of && n == row.of)
                out.push_back(row.name);
        if (const auto lock = locks.find(n); lock != locks.end())
            out.push_back(lock->second);
        if (own != part.end() && cls[n] == "Mesh" && depth < 8)
            for (const std::string &child : own->second.draw.children)
                follow(out, child, depth + 1);
    }

    // A Group's list: RndDrawable::Load (0x3d5090) adds a Draw 1's
    // children to the group it is loading and drops a Mesh's, so
    // each Mesh's follow it here. Its anims come last, for a View
    // it both draws and filters to end on the filter's frame.
    std::vector<std::string> Conversion::listed(const std::string &view)
    {
        std::vector<std::string> out;
        for (const std::string &child : part[view].draw.children)
            follow(out, child, 0);
        for (const std::string &a : part[view].anim.children)
            for (const std::string &as : stands(view, a))
                out.push_back(as);
        return out;
    }

    // GH2's own that no list here or of its own has draws last, over
    // GH1's scene, as its panel's later objects do.
    void Conversion::drawRest()
    {
        static const std::set<std::string> kDrawn = {
            "Group",  "Mesh",       "BandLabel", "BandButton", "BandPlacer",    "BandSlider",
            "UIList", "UIPicture",  "UIProxy",   "CheckBox",   "BandTextEntry",
        };
        std::set<std::string> held;
        for (const auto &[view, list] : lists)
            held.insert(list.begin(), list.end());
        for (size_t i = 0; i < gh2.entries.size(); ++i)
            if (gh2.entries[i].first == "Group" && filled.count(gh2.entries[i].second))
                for (const std::string &n : filled)
                    if (n != gh2.entries[i].second && holds(gh2.bodies[i], n))
                        held.insert(n);
        if (lists.count(top))
            for (const auto &[c, n] : gh2.entries)
                if (kDrawn.count(c) && filled.count(n) && !held.count(n))
                    lists[top].push_back(n);
    }

    // That object of GH1's as GH2's. False if it cannot be made.
    bool Conversion::object(size_t i)
    {
        const auto &[c, n] = gh1.entries[i];
        const Bytes &b = gh1.bodies[i];
        if (kOut.count(c))
            return true;
        if (c == "Tex")
            return addTex(n, b);
        else if (c == "Mat")
        {
            // A Mat 21 ends with its depth mode and five bytes. GH1 draws
            // one of mode 2 over what is there (store.gh's
            // pricesticker_02.mat, over the shop it stands behind); GH2's
            // z_mode 2 tests against it and writes none, and its 0 draws
            // over, as seen of each in the store.
            Bytes body = b;
            if (body.size() > 6u && body[body.size() - 6u] == 2u)
                body[body.size() - 6u] = 0u;
            milo::add(out, c, n, std::move(body));
        }
        else if (kAnims.count(c))
            addAnim(c, n, b);
        else if (const auto as = kKept.find(c); as != kKept.end())
            addKept(c, as->second, n, b);
        else if (c == "Mesh")
            addMesh(n, b);
        else if (c == "View")
            addView(n, b);
        else if (c == "Text")
            addText(n, b);
        else if (c == "LabelEx")
            return addLabel(n, b);
        else if (c == "ButtonEx")
            return addButton(n, b);
        else if (c == "PictureEx")
            return addPicture(n, b);
        return true;
    }

    std::optional<milo::Dir> convert(const milo::Dir &gh1, const milo::Dir &gh2, const Screen &screen, size_t disc,
                                     const std::string &top, const Look &look,
                                     const std::map<std::string, std::string> &down,
                                     const std::set<std::string> &scripted, std::map<std::string, std::string> &text)
    {
        return Conversion(gh1, gh2, screen, disc, top, look, down, scripted, text).run();
    }
}
