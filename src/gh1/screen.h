#pragma once

// One GH1 screen's scene as GH2's.

#include "gh1/look.h"
#include "milo/milo.h"

#include <array>
#include <cstddef>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace gh2::gh1
{
    // GH1's menu scenes and the files they name are under this.
    inline constexpr const char *kFolder = "ghui/";

    // A GH1 screen as one of GH2's.
    struct Screen
    {
        // GH1's scene, and GH2's where its name is another.
        const char *gh1, *gh2 = nullptr;
        // Its objects that GH2's script knows by another name, each a
        // start of a name.
        std::vector<std::pair<const char *, const char *>> renamed = {};
        // What GH2's scene has that this screen has no use for.
        std::vector<const char *> without = {};
        // Names GH2's script sets a frame by, each for the object here
        // that takes it.
        std::vector<std::pair<const char *, const char *>> frames = {};
        // What of GH2's art this screen keeps.
        std::vector<const char *> with = {};
        // Its buttons from the top down, each at that height under its
        // View and linked to the next. One with `of` is a button of
        // GH2's script that GH1's screen has no like of: `of`'s look,
        // with that text.
        struct Row
        {
            const char *name;
            float z;
            const char *of = nullptr, *text = nullptr;
        };
        std::vector<Row> rows = {};
        // Where GH2's own objects stand on this screen, and for one with
        // a scale, facing as the screen does at that size; one `grown`
        // keeps its own facing, that many times its size.
        struct Place
        {
            const char *name;
            float x, y, z, scale = 0.0f, grown = 0.0f;
        };
        std::vector<Place> placed = {};

        const char *ours() const { return gh2 ? gh2 : gh1; }
    };

    // One GH1 scene on its way to GH2's: what it draws and lists
    // (screen.cpp), each object as GH2's (screen_objects.cpp,
    // screen_widgets.cpp, screen_pictures.cpp), then what GH2's own scene
    // fills in (screen_gh2.cpp).
    class Conversion
    {
    public:
        Conversion(const milo::Dir &gh1, const milo::Dir &gh2, const Screen &screen, size_t disc, const std::string &top,
                   const Look &look, const std::map<std::string, std::string> &down,
                   const std::set<std::string> &scripted, std::map<std::string, std::string> &text)
            : gh1(gh1), gh2(gh2), screen(screen), disc(disc), top(top), look(look), scripted(scripted), text(text),
              linked(down)
        {
        }

        std::optional<milo::Dir> run();

    private:
        // What a BandButton takes of a ButtonEx 4 and its style.
        struct Button
        {
            uint32_t fit = 0u;
            bool caps = false;
            size_t box = 0u;
            std::string token, face;
            float size = 0.0f;
        };

        bool tree();
        void addRows();
        void fill();
        void reach(const std::string &n);
        bool readAnims();
        std::vector<std::string> stands(const std::string &view, const std::string &a);
        void follow(std::vector<std::string> &out, const std::string &n, int depth);
        std::vector<std::string> listed(const std::string &view);
        void drawRest();
        bool object(size_t i);
        void take(const std::string &name, int depth);
        void takeFromGh2();

        // Whether GH1's tree draws it.
        bool shows(const std::string &n, const Parts &p) const { return drawn.count(n) != 0u && p.draw.showing; }
        void putTrans(Bytes &out, const Bytes &b, const Parts &p);
        void putDraw(Bytes &out, const Bytes &b, const std::string &n, const Parts &p,
                     const std::vector<std::string> &children);
        void putComponent(Bytes &out, const Bytes &b, const std::string &n, const Parts &p, const std::string &type,
                          bool flat);
        void putText(Bytes &out, const std::string &token);
        bool addTex(const std::string &n, const Bytes &b);
        void addAnim(const std::string &c, const std::string &n, const Bytes &b);
        void addKept(const std::string &c, const std::string &as, const std::string &n, const Bytes &b);
        void addMesh(const std::string &n, const Bytes &b);
        void addView(const std::string &n, const Bytes &b);
        void addText(const std::string &n, const Bytes &b);
        bool addLabel(const std::string &n, const Bytes &b);
        bool addButton(const std::string &n, const Bytes &b);
        void colorButtons(const dtb::Node *style);
        Bytes bandButton(const std::string &name, const Screen::Row *row, const Bytes &b, const Parts &p,
                         const Button &button);
        static std::string lockOf(const std::string &picture);
        void readPicture(const std::string &n, const Bytes &b);
        void addResource(const std::string &cls, const std::string &name);
        bool addPictureMesh(const std::string &name, const std::string &picture, const Bytes &b, const char *key,
                            bool showing);
        bool addPicture(const std::string &n, const Bytes &b);

        const milo::Dir &gh1, &gh2;
        const Screen &screen;
        size_t disc;
        const std::string top;
        const Look &look;
        const std::set<std::string> &scripted;
        std::map<std::string, std::string> &text;

        std::map<std::string, Parts> part;
        std::map<std::string, std::string> cls, parent, linked;
        // Each PictureEx's style, and the name of its disabled Mesh.
        std::map<std::string, std::string> pictures, locks;
        std::map<std::string, const Screen::Row *> rows;
        std::set<std::string> filled, kept, drawn, wrapped, have;
        // Each anim's own Anim 0, and where its own fields start.
        std::map<std::string, std::pair<Anim, size_t>> anims;
        std::map<std::string, std::vector<std::string>> lists;
        milo::Dir out;
        bool colored = false;
        // Where each of GH2's placers placed here stood, and how far it
        // is moved.
        struct Move
        {
            std::array<float, 3> from, by;
        };
        std::vector<Move> moves;
    };

    // GH1's scene `gh1`, of the files on that disc, as GH2's `gh2`, drawn
    // from `top`.
    std::optional<milo::Dir> convert(const milo::Dir &gh1, const milo::Dir &gh2, const Screen &screen, size_t disc,
                                     const std::string &top, const Look &look,
                                     const std::map<std::string, std::string> &down,
                                     const std::set<std::string> &scripted, std::map<std::string, std::string> &text);
}
