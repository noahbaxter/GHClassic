// GH1's venue scripts as GH2's.
//
// GH1 scripts a venue in venues/<v>/<v>.dta: functions ({func set_lights_bad
// ...}) and handlers ({arena add_handlers (kick_drum ...) ...}) that set its
// anims going, {arena switch_anim <anim> (loop a b) (scale s) (blend t)},
// from where the song is and how it goes (arena/venue.dta). An anim's frames
// are the song's ticks, 480 a beat, and a blend's time too (VenueAnim::Set,
// GH1 0x17cc28).
//
// GH2 scripts one as its world's type (world/<v>/<v>.dta, merged into
// world_objects.dta), whose handlers the game's events reach: intro_start,
// verse, chorus, solo, excitement_bad, kick_drum, bass_hit, hit_p0_fret1 and
// the rest of macros.dta's WORLDEVENTS. So GH1's functions and handlers are
// handlers of the type of the GH2 venue standing in for it, each
// switch_anim an {<anim> animate (loop a b) (period p) (blend t)}
// (RndAnimatable::OnAnimate, 0x1abf00) of what stands for that anim
// (gh1/venues.cpp), and arena/venue.dta's choice of lights with them.
//
// What has nothing of GH2's to be is left out: a command on an object not
// brought in, sounds, and GH1's game_lost, which GH2's own handler answers.

#include "gh1/scripts.h"

#include "disc/ark.h"
#include "formats/dtb.h"

#include <cmath>
#include <iostream>
#include <optional>

namespace gh2::gh1
{
    namespace
    {
        using dtb::Node;

        Node symbol(const std::string &text) { return {dtb::kSymbol, 0, 0.0f, text, {}}; }
        Node real(float v) { return {dtb::kFloat, 0, v, {}, {}}; }
        Node array(std::vector<Node> nodes) { return {dtb::kArray, 0, 0.0f, {}, std::move(nodes)}; }
        Node command(std::vector<Node> nodes) { return {dtb::kCommand, 0, 0.0f, {}, std::move(nodes)}; }
        Node property(const std::string &name) { return {dtb::kProperty, 0, 0.0f, {}, {symbol(name)}}; }

        bool is(const Node &node, const char *text)
        {
            return node.type == dtb::kSymbol && node.text == text;
        }

        // GH1's ticks to a beat.
        constexpr float kTicks = 480.0f;

        // A venue's script: its functions and its handlers, each a name,
        // its arguments if it takes any, and its body.
        std::optional<Node> read(size_t disc, const std::string &gh1)
        {
            const dtb::Files files = [disc](const std::string &file) { return ark::readFile(disc, file); };
            dtb::Macros macros;
            return dtb::read("venues/" + gh1 + "/" + gh1 + ".dta", macros, files);
        }

        struct Script
        {
            std::vector<Node> functions; // (name (args) body...)
            std::vector<Node> handlers;  // (name body...)
        };

        Script parts(const Node &script)
        {
            Script out;
            for (const Node &top : script.nodes)
            {
                if (top.type != dtb::kCommand || top.nodes.size() < 2u)
                    continue;
                if (is(top.nodes[0], "func"))
                    out.functions.push_back(array({top.nodes.begin() + 1, top.nodes.end()}));
                else if (is(top.nodes[0], "arena") && is(top.nodes[1], "add_handlers"))
                    for (size_t i = 2u; i < top.nodes.size(); ++i)
                        if (top.nodes[i].type == dtb::kArray && !top.nodes[i].nodes.empty())
                            out.handlers.push_back(top.nodes[i]);
            }
            return out;
        }

        void collect(const Node &node, std::set<std::string> &out)
        {
            if (node.type == dtb::kCommand && node.nodes.size() > 2u)
            {
                const Node &head = node.nodes[0], &what = node.nodes[1];
                if ((is(head, "arena") || is(head, "game")) &&
                    (is(what, "switch_anim") || is(what, "switch_anim_rt") || is(what, "anim_task")) &&
                    node.nodes[2].type == dtb::kSymbol)
                    out.insert(node.nodes[2].text);
                if (is(what, "remove_anim") && node.nodes[2].type == dtb::kSymbol)
                    out.insert(node.nodes[2].text);
            }
            if (node.type == dtb::kCommand && node.nodes.size() == 2u && is(node.nodes[1], "unhook_anim_parents"))
                out.insert(node.nodes[0].text);
            for (const Node &child : node.nodes)
                collect(child, out);
        }

        struct Venue
        {
            const Drivers &drivers;
            const std::set<std::string> &objects;
            std::set<std::string> functions; // and handlers, by name
        };

        // `key`'s array among a command's options, or none.
        const Node *option(const Node &cmd, size_t from, const char *key)
        {
            for (size_t i = from; i < cmd.nodes.size(); ++i)
                if (cmd.nodes[i].type == dtb::kArray && !cmd.nodes[i].nodes.empty() && is(cmd.nodes[i].nodes[0], key))
                    return &cmd.nodes[i];
            return nullptr;
        }

        std::vector<Node> translate(const Node &node, const Venue &venue);

        std::vector<Node> translated(const std::vector<Node> &nodes, size_t from, const Venue &venue)
        {
            std::vector<Node> out;
            for (size_t i = from; i < nodes.size(); ++i)
            {
                std::vector<Node> made = translate(nodes[i], venue);
                out.insert(out.end(), std::make_move_iterator(made.begin()), std::make_move_iterator(made.end()));
            }
            return out;
        }

        // The objects standing for the anim a command names, itself if it
        // names it by a variable.
        std::vector<Node> targets(const Node &named, const Venue &venue)
        {
            if (named.type != dtb::kSymbol)
                return {named};
            std::vector<Node> out;
            if (const auto it = venue.drivers.find(named.text); it != venue.drivers.end())
                for (const std::string &driver : it->second)
                    out.push_back(symbol(driver));
            return out;
        }

        // That object in the room's dir. A name alone finds the first in the
        // world's dirs, and the stand-in's have some of GH1's names: its
        // chars dir a crowd.env, the theatre's lighting dir a
        // spotlight01.lit and spotlight01.tnm.
        Node inRoom(const std::string &name)
        {
            return command({command({symbol("venue.view"), symbol("dir")}), symbol("find"), symbol(name)});
        }

        // Those of GH2's characters that are there drawn under that Environ
        // of the venue's (RndDir's environ, 0x1b3de0). GH1 draws each
        // guitarist under singer<n>.env, or the one its script last set,
        // the rest of the band under stagechar.env (MyCharSys::Draw, GH1
        // 0x2826d0) and the crowd under crowd.env (Arena::SetupEnvs, GH1
        // 0x168370).
        std::vector<Node> lit(std::initializer_list<const char *> characters, const std::string &by)
        {
            std::vector<Node> out;
            for (const char *who : characters)
                out.push_back(command({symbol("if"), command({symbol("exists"), symbol(who)}),
                                       command({symbol(who), symbol("set"), symbol("environ"), inRoom(by)})}));
            return out;
        }

        // {arena switch_anim <anim> (loop a b) (scale s) (blend t)}, or
        // switch_anim_rt in milliseconds, as {<anim> animate ...}: a range
        // from one frame to another, played once or looped, at the anim's
        // 480 frames a beat unless a scale or the clock gives a period.
        std::vector<Node> animate(const Node &cmd, bool realTime, const Venue &venue)
        {
            std::vector<Node> out;
            const Node *loop = option(cmd, 3u, "loop"), *range = option(cmd, 3u, "range");
            const Node *span = loop ? loop : range;
            if (cmd.nodes.size() < 4u || !span || span->nodes.size() < 3u)
                return out;
            const auto number = [&](const char *key, float fallback)
            {
                const Node *found = option(cmd, 3u, key);
                const auto v = found && found->nodes.size() > 1u ? dtb::number(found->nodes[1]) : std::nullopt;
                return v ? *v : fallback;
            };
            const float scale = number("scale", 1.0f), blend = number("blend", 0.0f);
            const float unit = realTime ? 1000.0f : kTicks;
            const auto from = dtb::number(span->nodes[1]), to = dtb::number(span->nodes[2]);
            const bool still = from && to && *from == *to;
            std::vector<Node> ends = translated(span->nodes, 1u, venue);
            if (ends.size() != 2u)
                return out;
            for (const Node &target : targets(cmd.nodes[2], venue))
            {
                Node made = command({target, symbol("animate")});
                // A loop of no length holds its frame (GH1 0x17cf18).
                made.nodes.push_back(array({symbol(loop && !still ? "loop" : "range"), ends[0], ends[1]}));
                if (realTime)
                    made.nodes.push_back(array({symbol("units"), {dtb::kInt, 0, 0.0f, {}, {}}}));
                if (from && to && !still && scale != 0.0f && (scale != 1.0f || realTime))
                    made.nodes.push_back(array({symbol("period"), real(std::fabs(*to - *from) / (unit * std::fabs(scale)))}));
                if (blend != 0.0f)
                    made.nodes.push_back(array({symbol("blend"), real(blend / unit)}));
                out.push_back(std::move(made));
            }
            return out;
        }

        std::vector<Node> translate(const Node &node, const Venue &venue)
        {
            static const std::set<std::string> kControl = {"if", "if_else", "unless", "foreach", "do", "switch"};
            static const std::set<std::string> kMethods = {"set_showing", "set_frame", "set_steps"};
            static const std::set<std::string> kKept = {
                "set", "random_int", "random_float", "exists", "==", "!=", ">", "<", ">=", "<=", "!", "&&", "||",
                "+", "-", "*", "/",
            };
            switch (node.type)
            {
            case dtb::kVar:
                if (node.text == "arena.excitement")
                    return {property("excitement_level")};
                return {node};
            case dtb::kSymbol:
            {
                // An anim, wherever it is named, is what stands for it.
                const auto it = venue.drivers.find(node.text);
                if (it == venue.drivers.end())
                    return {node};
                std::vector<Node> out;
                for (const std::string &driver : it->second)
                    out.push_back(symbol(driver));
                return out;
            }
            case dtb::kArray:
            {
                Node out = node;
                out.nodes = translated(node.nodes, 0u, venue);
                return {out};
            }
            case dtb::kCommand:
                break;
            default:
                return {node};
            }
            if (node.nodes.empty())
                return {};
            const Node &head = node.nodes[0];
            if (head.type == dtb::kVar)
            {
                // A method of whatever the variable names.
                Node out = node;
                out.nodes = translated(node.nodes, 1u, venue);
                out.nodes.insert(out.nodes.begin(), head);
                return {out};
            }
            // {<cam> add_trans <object>} hangs the object off GH1's one
            // camera, which GH2's default.cam is here: the theatre's rim
            // light, so behind the band from wherever a shot looks.
            if (node.nodes.size() == 3u && node.nodes[1].type == dtb::kSymbol && node.nodes[1].text == "add_trans" &&
                venue.objects.count(node.nodes[2].text))
                return {command({symbol(node.nodes[2].text), symbol("set"), symbol("trans_parent"),
                                 command({{dtb::kVar, 0, 0.0f, "this", {}}, symbol("find"), symbol("default.cam")})})};
            // {with_namespace {<member> geom_space} {top.view set_showing x}}
            // hides a band member from a shot that stands in it: top.view is
            // the View all of the member draws under (charsys/<member>).
            if (head.type == dtb::kSymbol && head.text == "with_namespace" && node.nodes.size() == 3u &&
                node.nodes[1].type == dtb::kCommand && node.nodes[1].nodes.size() == 2u &&
                node.nodes[1].nodes[1].text == "geom_space" && node.nodes[2].type == dtb::kCommand &&
                node.nodes[2].nodes.size() == 3u && node.nodes[2].nodes[0].text == "top.view" &&
                node.nodes[2].nodes[1].text == "set_showing")
            {
                const Node who = node.nodes[1].nodes[0];
                const std::vector<Node> showing = translate(node.nodes[2].nodes[2], venue);
                if (showing.size() != 1u)
                    return {};
                return {command({symbol("if"), command({symbol("exists"), who}),
                                 command({who, symbol("set_showing"), showing[0]})})};
            }
            if (head.type != dtb::kSymbol)
                return {};
            // arena::<object> is the venue's, as every object here is.
            const std::string h = head.text.rfind("arena::", 0) == 0 ? head.text.substr(7u) : head.text;
            const std::string what = node.nodes.size() > 1u && node.nodes[1].type == dtb::kSymbol ? node.nodes[1].text : "";
            // {char_sys get_spot <guitarist>}: the walk spot it is nearest, by
            // its number less one (CharMan::GetSpot, GH1 0x18ef80).
            if (h == "char_sys" && what == "get_spot" && node.nodes.size() == 3u)
                return {command({{dtb::kVar, 0, 0.0f, "this", {}}, symbol("gh1_spot"), node.nodes[2]})};
            if (h == "arena" || h == "game")
            {
                if (what == "switch_anim" || what == "switch_anim_rt")
                    return animate(node, what == "switch_anim_rt", venue);
                if (what == "anim_task" && node.nodes.size() > 5u)
                {
                    // {game anim_task <anim> <time> <from> <to>}
                    // (TaskMgr::OnAnimTask, GH1 0x242b48): the arena's
                    // tasks run in ticks, the game panel's in milliseconds
                    // (UIPanel::Poll, GH1 0x207140).
                    std::vector<Node> out;
                    const auto time = dtb::number(node.nodes[3]);
                    for (const Node &target : targets(node.nodes[2], venue))
                    {
                        Node made = command({target, symbol("animate"), array({symbol("range"), node.nodes[4], node.nodes[5]})});
                        if (h == "game")
                            made.nodes.push_back(array({symbol("units"), {dtb::kInt, 0, 0.0f, {}, {}}}));
                        made.nodes.push_back(array({symbol("period"), real(h == "game" ? (time ? *time : 1000.0f) / 1000.0f
                                                                                        : (time ? *time : kTicks) / kTicks)}));
                        out.push_back(std::move(made));
                    }
                    return out;
                }
                if (what == "delay_task" && node.nodes.size() > 3u)
                {
                    // {arena delay_task <ticks> ...}
                    const auto ticks = dtb::number(node.nodes[2]);
                    std::vector<Node> body = translated(node.nodes, 3u, venue);
                    if (!ticks || body.empty())
                        return {};
                    body.insert(body.begin(), symbol("script"));
                    return {command({symbol("script_task"), array({symbol("units"), symbol("kTaskBeats")}),
                                     array({symbol("delay"), real(*ticks / kTicks)}), array(std::move(body))})};
                }
                // The shot's name, as GH1's script tells shots apart.
                if (h == "arena" && what == "cam_msg")
                    return {command({command({{dtb::kVar, 0, 0.0f, "this", {}}, symbol("current_shot")}), symbol("name")})};
                // The Environ both guitarists are drawn under from now on.
                if (h == "arena" && what == "set_singer_env" && node.nodes.size() > 2u &&
                    venue.objects.count(node.nodes[2].text))
                    return lit({"guitarist0", "guitarist1"}, node.nodes[2].text);
                if (h == "game" && what == "multiplayer")
                    return {node};
                if (h == "arena" && venue.functions.count(what))
                {
                    Node out = command({{dtb::kVar, 0, 0.0f, "this", {}}, symbol(what)});
                    std::vector<Node> args = translated(node.nodes, 2u, venue);
                    out.nodes.insert(out.nodes.end(), args.begin(), args.end());
                    return {out};
                }
                return {};
            }
            if (h == "animate_to" && node.nodes.size() > 4u)
            {
                // {animate_to arena <anim> <to> <ticks>}: from the frame it
                // is at (system_script.dta).
                std::vector<Node> out;
                const auto ticks = dtb::number(node.nodes[4]);
                for (const Node &target : targets(node.nodes[2], venue))
                    out.push_back(command({target, symbol("animate"),
                                           array({symbol("range"), command({target, symbol("frame")}), node.nodes[3]}),
                                           array({symbol("period"), real((ticks ? *ticks : kTicks) / kTicks)})}));
                return out;
            }
            if (venue.functions.count(h))
            {
                Node out = command({{dtb::kVar, 0, 0.0f, "this", {}}, symbol(h)});
                std::vector<Node> args = translated(node.nodes, 1u, venue);
                out.nodes.insert(out.nodes.end(), args.begin(), args.end());
                return {out};
            }
            if (kControl.count(h))
            {
                // What it tests, or runs over, then what it does.
                const size_t fixed = h == "foreach" ? 3u : h == "do" ? 1u : 2u;
                if (node.nodes.size() < fixed)
                    return {};
                Node out = command({head});
                for (size_t i = 1u; i < fixed; ++i)
                {
                    std::vector<Node> made = translate(node.nodes[i], venue);
                    if (made.size() != 1u)
                        return {};
                    out.nodes.push_back(std::move(made[0]));
                }
                std::vector<Node> body = translated(node.nodes, fixed, venue);
                if (body.empty())
                    return {};
                out.nodes.insert(out.nodes.end(), body.begin(), body.end());
                return {out};
            }
            if (kKept.count(h))
            {
                Node out = command({head});
                for (size_t i = 1u; i < node.nodes.size(); ++i)
                {
                    std::vector<Node> made = translate(node.nodes[i], venue);
                    if (made.size() != 1u)
                        return {};
                    out.nodes.push_back(std::move(made[0]));
                }
                return {out};
            }
            // A method of an object: one GH2's objects answer, of an object
            // that is here, or a frame for each that stands for an anim.
            if (!kMethods.count(what))
                return {};
            std::vector<Node> out;
            std::vector<Node> objects;
            if (what == "set_frame" && venue.drivers.count(h))
                for (const Node &driver : targets(symbol(h), venue))
                    objects.push_back(inRoom(driver.text));
            else if (venue.objects.count(h))
                objects.push_back(inRoom(what == "set_showing" && venue.objects.count(drawsOf(h)) ? drawsOf(h) : h));
            const std::vector<Node> with = translated(node.nodes, 2u, venue);
            if (with.size() + 2u != node.nodes.size())
                return {};
            for (const Node &object : objects)
            {
                Node made = command({object, node.nodes[1]});
                made.nodes.insert(made.nodes.end(), with.begin(), with.end());
                out.push_back(std::move(made));
            }
            return out;
        }

        // arena/venue.dta's: where the song is and how it goes pick the
        // lights, on the downbeat after either changes (GH2 has no event
        // for one: the next beat that is a multiple of four), the intro has
        // the bad ones and the first shot after it the music's start, which
        // GH2 has no event for. A beat is GH2's own with GH1's cut from a
        // shot the guitarist may not walk in once he walks
        // (CameraShot::Okay, GH1 0x110040), which GH2's check leaves out
        // (CheckShot, 0x11f628).
        constexpr const char *kGlue = R"(
(beat
 {set [camera_beat] $beat}
 {if {world current_shot}
  {if {|| {! {{world current_shot} check_shot}}
       {&& {guitarist0 actually_walking} {! {{world current_shot} get walk_ok}}}}
   {$this pick_new_shot}}})
(gh1_section verse)
(gh1_started FALSE)
(gh1_lights_due FALSE)
(gh1_lights_soon
 {if {! [gh1_lights_due]}
  {set [gh1_lights_due] TRUE}
  {script_task (units kTaskBeats)
   (delay {- {* 4 {+ 1 {int {/ {taskmgr beat} 4}}}} {taskmgr beat}})
   (script {world set gh1_lights_due FALSE} {world gh1_lights})}})
(excitement_bad {$this gh1_lights_soon})
(excitement_okay {$this gh1_lights_soon})
(excitement_great {$this gh1_lights_soon})
(gh1_lights
 {if_else {game multiplayer}
  {$this gh1_lights_great}
  {switch [excitement_level]
   (kExcitementBoot {$this set_lights_bad})
   (kExcitementBad {$this set_lights_bad})
   (kExcitementOkay
    {switch [gh1_section]
     (verse {$this set_lights_okay_verse})
     (chorus {$this set_lights_okay_chorus})
     (solo {$this set_lights_okay_solo})})
   (kExcitementGreat {$this gh1_lights_great})
   (kExcitementPeak {$this gh1_lights_great})}})
(gh1_lights_great
 {switch [gh1_section]
  (verse {$this set_lights_great_verse})
  (chorus {$this set_lights_great_chorus})
  (solo {$this set_lights_great_solo})})
(verse {set [gh1_section] verse} {$this gh1_lights_soon})
(chorus {set [gh1_section] chorus} {$this gh1_lights_soon})
(solo {set [gh1_section] solo} {$this gh1_lights_soon})
(intro_start
 {set [gh1_started] FALSE}
 {set [gh1_lights_due] FALSE}
 {set [gh1_section] verse}
 {$this gh1_scene}
 {$this set_lights_bad})
(post_switch_cam
 {if {&& {! [gh1_started]} {world current_shot}}
  {if {!= {{world current_shot} get category} INTRO}
   {set [gh1_started] TRUE}
   {$this set_lights_okay_verse}
   {$this gh1_music_start}}})
(game_won {$this set_lights_great_verse})
(hit_p0_fret1 {$this hit_gem 0})
(hit_p0_fret2 {$this hit_gem 1})
(hit_p0_fret3 {$this hit_gem 2})
(hit_p0_fret4 {$this hit_gem 3})
(hit_p0_fret5 {$this hit_gem 4})
)";

        // The array in `in` that starts with that symbol.
        Node *child(Node &in, const char *key)
        {
            for (Node &n : in.nodes)
                if (n.type == dtb::kArray && !n.nodes.empty() && is(n.nodes[0], key))
                    return &n;
            return nullptr;
        }

        // GH1's own settings for the venue (arena/venues.dta) over the
        // stand-in's in its world's type: each crowd stream and how loud it
        // is, a gain there and decibels here, how many bars a shot lasts by
        // how the song goes, and where the intro is shot from.
        void configure(Node &type, const Node &theirs)
        {
            const auto decibels = [](const Node &gain)
            {
                const auto v = dtb::number(gain);
                return real(v && *v > 0.0f ? 20.0f * std::log10(*v) : -96.0f);
            };
            const auto stream = [&](Node &ours, const Node *from)
            {
                if (from && from->nodes.size() > 2u && ours.nodes.size() > 2u)
                {
                    ours.nodes[1] = decibels(from->nodes[1]);
                    ours.nodes[2] = from->nodes[2];
                }
            };
            const Node *sound = dtb::find(theirs, "sound");
            const Node *crowd = sound ? dtb::find(*sound, "crowd") : nullptr;
            Node *ourSound = child(type, "sound");
            Node *ourCrowd = ourSound ? child(*ourSound, "crowd") : nullptr;
            if (crowd && ourCrowd)
            {
                if (Node *intro = child(*ourCrowd, "intro"))
                    stream(*intro, dtb::find(*crowd, "intro"));
                const Node *levels = dtb::find(*crowd, "levels");
                Node *ourLevels = child(*ourCrowd, "levels");
                for (size_t i = 1u; levels && ourLevels && i < levels->nodes.size() && i < ourLevels->nodes.size(); ++i)
                    stream(ourLevels->nodes[i], &levels->nodes[i]);
            }
            const Node *bars = dtb::find(theirs, "camera_durations");
            Node *ourBars = child(type, "camera_durations");
            if (bars && ourBars && ourBars->nodes.size() > 1u)
            {
                Node &list = ourBars->nodes[1];
                for (size_t i = 0u; i < list.nodes.size() && i + 1u < bars->nodes.size(); ++i)
                    if (list.nodes[i].nodes.size() > 2u && bars->nodes[i + 1u].nodes.size() > 2u)
                    {
                        list.nodes[i].nodes[1] = bars->nodes[i + 1u].nodes[1];
                        list.nodes[i].nodes[2] = bars->nodes[i + 1u].nodes[2];
                    }
            }
            if (const Node *flags = dtb::find(theirs, "intro_camera_flags"); flags && flags->nodes.size() > 1u)
                for (const Node &flag : flags->nodes[1].nodes)
                {
                    const bool distance = is(flag, "kCamNear") || is(flag, "kCamFar");
                    if (!distance && !is(flag, "kCamLeft") && !is(flag, "kCamRight"))
                        continue;
                    if (Node *ours = child(type, distance ? "intro_camera_distance" : "intro_camera_facing"); ours && ours->nodes.size() > 1u)
                        ours->nodes[1] = symbol(is(flag, "kCamNear") ? "near" : is(flag, "kCamFar") ? "far" : is(flag, "kCamLeft") ? "left" : "right");
                }
        }

        // GH1's name for a handler as the one of GH2's that runs it.
        std::string renamed(const std::string &handler)
        {
            return handler == "finish_loading" ? "intro_start" : handler == "music_start" ? "gh1_music_start" : handler;
        }
    }

    std::set<std::string> scripted(size_t disc, const std::string &gh1)
    {
        std::set<std::string> out;
        if (const auto script = read(disc, gh1))
            collect(*script, out);
        return out;
    }

    void addScripts(size_t layer, size_t disc, const std::string &gh1, const std::string &gh2, const Drivers &drivers,
                    const std::set<std::string> &objects, const std::string &kit, const std::vector<std::string> &spots)
    {
        const std::string path = "world/" + gh2 + "/gen/" + gh2 + ".dtb";
        const auto file = ark::readFile(0u, path);
        auto root = file ? dtb::raw(*file) : std::nullopt;
        const auto script = read(disc, gh1);
        if (!root || !script)
        {
            std::cerr << "[gh1] cannot read " << gh1 << "'s script" << std::endl;
            return;
        }
        const Script theirs = parts(*script);
        Venue venue{drivers, objects, {}};
        for (const Node &f : theirs.functions)
            venue.functions.insert(f.nodes[0].text);
        for (const Node &h : theirs.handlers)
            venue.functions.insert(h.nodes[0].text);

        // Each handler once: the glue's, then GH1's functions and handlers
        // under their names, a handler that only calls the function of its
        // own name left out for it.
        std::vector<Node> made = dtb::parse(kGlue)->nodes;
        // The walk spot whose waypoint is the nearest to that character of
        // those it walks to (kWalkSpot or kSoloWalkSpot, 0x191078), -1 with
        // none.
        std::string nearest = "(gh1_spot ($who) {do ($at {waypoint_nearest $who 192}) {if_else {== $at \"\"} -1 {- {switch {$at name}";
        for (size_t i = 0; i < spots.size(); ++i)
            nearest += " (" + spots[i] + " " + std::to_string(i + 1u) + ")";
        made.push_back(dtb::parse(nearest + "} 1}}})")->nodes[0]);
        const auto handler = [&](const std::string &name) -> Node &
        {
            for (Node &m : made)
                if (!m.nodes.empty() && m.nodes[0].text == name)
                    return m;
            made.push_back(array({symbol(name)}));
            return made.back();
        };
        std::set<std::string> functions;
        for (const Node &f : theirs.functions)
        {
            if (f.nodes.size() < 2u)
                continue;
            functions.insert(f.nodes[0].text);
            Node &to = handler(f.nodes[0].text);
            const size_t body = f.nodes[1].type == dtb::kArray ? 2u : 1u;
            if (body == 2u && !f.nodes[1].nodes.empty())
                to.nodes.push_back(f.nodes[1]);
            std::vector<Node> does = translated(f.nodes, body, venue);
            to.nodes.insert(to.nodes.end(), does.begin(), does.end());
        }
        for (const Node &h : theirs.handlers)
        {
            const std::string &name = h.nodes[0].text;
            if (functions.count(name) || name == "terminate" || name == "game_lost")
                continue;
            Node &to = handler(renamed(name));
            if (name == "hit_gem" && to.nodes.size() == 1u)
                to.nodes.push_back(array({{dtb::kVar, 0, 0.0f, "slot", {}}}));
            std::vector<Node> does = translated(h.nodes, 1u, venue);
            // GH2 sends it as a shot starts (world/camshot.dta), and once
            // with none.
            if (name == "post_switch_cam" && !does.empty())
            {
                does.insert(does.begin(), {symbol("if"), command({{dtb::kVar, 0, 0.0f, "this", {}}, symbol("current_shot")})});
                does = {command(std::move(does))};
            }
            to.nodes.insert(to.nodes.end(), does.begin(), does.end());
        }
        // The scene's frame is the song's tick from its start, as far as a
        // song goes; the functions the glue calls are there for it to call.
        Node &scene = handler("gh1_scene");
        // GH1 draws each section's View, the crowd, the band, then each
        // section's transparent View (ArenaPanel::Draw, GH1 0x10d488), the
        // guitarist's pool of light before those (Arena::DrawTransparent,
        // GH1 0x169818). GH2's crowd draws at 0 and its band from 6. A dir
        // sorts its draws when it syncs alone (RndDir::SyncObjects,
        // 0x1b2f78).
        static const std::pair<const char *, float> kOrder[] = {
            {"venue.view", -2.0f}, {"lighting.view", -1.0f}, {"floorspot_char.mesh", 9.0f},
            {"venue_transparent.view", 10.0f}, {"lighting_transparent.view", 11.0f},
        };
        for (const auto &[view, order] : kOrder)
            if (objects.count(view))
                scene.nodes.push_back(command({symbol(view), symbol("set"), symbol("draw_order"), real(order)}));
        scene.nodes.push_back(command({{dtb::kVar, 0, 0.0f, "this", {}}, symbol("sync_objects")}));
        // The pool follows the first guitarist (content/floor_spot.h).
        if (objects.count("floorspot_char.mesh"))
            scene.nodes.push_back(command({symbol("if"), command({symbol("exists"), symbol("guitarist0")}),
                                           command({symbol("floor_spot"), symbol("floorspot_char.mesh"),
                                                    inRoom("spotlight01.lit"), symbol("guitarist0")})}));
        if (objects.count(kit))
            scene.nodes.push_back(command({symbol(kit), symbol("set_showing"), command({symbol("band"), symbol("room_kit")})}));
        for (const char *top : {"venue.view", "lighting.view"})
            if (const auto it = drivers.find(top); it != drivers.end())
                for (const std::string &driver : it->second)
                    scene.nodes.push_back(command({symbol(driver), symbol("animate"),
                                                   array({symbol("range"), real(0.0f), real(1.0e7f)})}));
        for (const auto &[who, by] : {std::pair{"guitarist0", "singer0.env"}, std::pair{"guitarist1", "singer1.env"}})
            if (objects.count(by))
                for (Node &made : lit({who}, by))
                    scene.nodes.push_back(std::move(made));
        if (objects.count("stagechar.env"))
            for (Node &made : lit({"singer", "bassist", "drummer", "keyboardist"}, "stagechar.env"))
                scene.nodes.push_back(std::move(made));
        // GH1 shows no shadow of the band's or a guitarist's own: where a
        // room has them it draws them, and a guitarist's is drawn with no
        // alpha, which its alpha test keeps to the Z buffer. A setting
        // picks the room's, GH2's own under everyone, or neither. They are
        // only ever hidden here: a band member's shadow.mesh, wherever the
        // room draws the band's, and a guitarist's Group of them, as GH2
        // hides it with two players (char_objects.dta).
        const auto chosen = [](const char *which)
        { return command({symbol("=="), command({symbol("band"), symbol("shadows")}), symbol(which)}); };
        const Node no = {dtb::kInt, 0, 0.0f, {}, {}};
        for (const char *who : {"singer", "bassist", "drummer", "keyboardist"})
        {
            Node hide = command({symbol("if"), command({symbol(who), symbol("exists"), symbol("shadow.mesh")}),
                                 command({command({symbol(who), symbol("find"), symbol("shadow.mesh")}), symbol("set_showing"), no})});
            if (!objects.count("band_shadow.mesh"))
                hide = command({symbol("if"), command({symbol("!"), chosen("gh2")}), std::move(hide)});
            scene.nodes.push_back(command({symbol("if"), command({symbol("exists"), symbol(who)}), std::move(hide)}));
        }
        for (const std::string who : {"guitarist0", "guitarist1"})
            scene.nodes.push_back(dtb::parse("{if {&& {exists " + who + "} {! {== {band shadows} gh2}}} {do ($group {" + who +
                                             " get shadow}) {if {!= $group \"\"} {$group set_showing FALSE}}}}")
                                      ->nodes[0]);
        for (const std::string &object : objects)
            if (object.rfind("band_shadow", 0) == 0)
                scene.nodes.push_back(command({symbol("if"), chosen("off"), command({symbol(object), symbol("set_showing"), no})}));
        if (objects.count("crowd.env"))
            for (Node &made : lit({"crowd_male01", "crowd_male02", "crowd_male03", "crowd_male04", "crowd_female01",
                                   "crowd_female02", "crowd_female03", "crowd_female04"},
                                  "crowd.env"))
                scene.nodes.push_back(std::move(made));
        for (const char *name : {"gh1_music_start", "hit_gem", "set_lights_bad", "set_lights_okay_verse", "set_lights_okay_chorus",
                                 "set_lights_okay_solo", "set_lights_great_verse", "set_lights_great_chorus",
                                 "set_lights_great_solo"})
            handler(name);
        for (Node &m : made)
            if (m.nodes.size() == 1u || (m.nodes.size() == 2u && m.nodes[1].type == dtb::kArray))
                m.nodes.push_back({dtb::kInt, 0, 0.0f, {}, {}});

        const dtb::Files files = [disc](const std::string &f) { return ark::readFile(disc, f); };
        dtb::Macros macros;
        const auto venues = dtb::read("arena/venues.dta", macros, files);
        const Node *settings = venues ? dtb::find(*venues, gh1) : nullptr;

        bool placed = false;
        for (Node &cls : root->nodes)
            if (cls.type == dtb::kArray && !cls.nodes.empty() && is(cls.nodes[0], "WorldDir"))
                for (Node &types : cls.nodes)
                    if (types.type == dtb::kArray && !types.nodes.empty() && is(types.nodes[0], "types"))
                        for (Node &type : types.nodes)
                            if (type.type == dtb::kArray && !type.nodes.empty() && is(type.nodes[0], gh2.c_str()))
                            {
                                if (settings)
                                    configure(type, *settings);
                                // Ahead of the stand-in's own and its base's:
                                // the first of a name is the one found.
                                type.nodes.insert(type.nodes.begin() + 1, made.begin(), made.end());
                                placed = true;
                            }
        if (!placed)
        {
            std::cerr << "[gh1] no type for " << gh2 << "'s world" << std::endl;
            return;
        }
        ark::addFile(layer, path, dtb::write(*root));
    }
}
