#include "gh2x/scene.h"

#include "gh2x/classes.h"
#include "gh2x/focus.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <iostream>
#include <map>
#include <set>
#include <tuple>
#include <vector>

namespace gh2::gh2x
{
    namespace
    {
        using milo::Bytes;

        // Every ng folder in a path as og.
        std::string ogFolders(std::string path)
        {
            for (size_t at = path.find("ng"); at != std::string::npos; at = path.find("ng", at + 2u))
            {
                const bool start = at == 0u || path[at - 1u] == '/';
                const bool end = at + 2u == path.size() || path[at + 2u] == '/';
                if (start && end)
                    path[at] = 'o';
            }
            return path;
        }

        // `path` taken from the folder `base`, with . and .. resolved.
        std::string resolve(const std::string &base, const std::string &path)
        {
            std::vector<std::string> parts;
            const std::string joined = path.starts_with('/') ? path : base + "/" + path;
            size_t at = 0u;
            while (at <= joined.size())
            {
                const size_t end = std::min(joined.find('/', at), joined.size());
                const std::string part = joined.substr(at, end - at);
                if (part == ".." && !parts.empty() && parts.back() != "..")
                    parts.pop_back();
                else if (!part.empty() && part != ".")
                    parts.push_back(part);
                at = end + 1u;
            }
            std::string out;
            for (const std::string &part : parts)
                out += (out.empty() ? "" : "/") + part;
            return out;
        }

        // Whether a .milo path written in a scene names a file the game can
        // have: from the scene's folder, the file itself one gen/ down (../og/
        // battle_lighting.milo in world/battle/gen is world/battle/og/gen/
        // battle_lighting.milo_ps2).
        bool haveScene(const SceneSource &source, const std::string &base, const std::string &path)
        {
            std::string forward = path;
            std::replace(forward.begin(), forward.end(), '\\', '/');
            const std::string full = resolve(base, forward);
            const size_t slash = full.rfind('/');
            const std::string folder = slash == std::string::npos ? "" : full.substr(0u, slash + 1u);
            const std::string name = full.substr(slash == std::string::npos ? 0u : slash + 1u);
            std::string file = folder + "gen/" + name + "_ps2";
            std::transform(file.begin(), file.end(), file.begin(), [](unsigned char c) { return std::tolower(c); });
            return source.has(file);
        }

        // Each string in a body: a run of two or more printable bytes that
        // the word before it gives the length of. The ng folders they name
        // become og, but a scene path only where the game can have the file.
        Bytes fixPaths(const SceneSource &source, const std::string &base, const Bytes &body)
        {
            Bytes out;
            size_t copied = 0u;
            size_t o = 0u;
            while (o < body.size())
            {
                if (body[o] < 0x20u || body[o] > 0x7eu)
                {
                    ++o;
                    continue;
                }
                size_t end = o;
                while (end < body.size() && body[end] >= 0x20u && body[end] <= 0x7eu)
                    ++end;
                const size_t n = end - o;
                uint32_t length = 0u;
                if (o >= 4u)
                    std::memcpy(&length, body.data() + o - 4u, 4u);
                if (n >= 2u && o >= 4u && length == n && o - 4u >= copied)
                {
                    const std::string text(reinterpret_cast<const char *>(body.data() + o), n);
                    const std::string og = ogFolders(text);
                    if (og != text && (og.find(".milo") == std::string::npos || haveScene(source, base, og)))
                    {
                        out.insert(out.end(), body.begin() + static_cast<std::ptrdiff_t>(copied),
                                   body.begin() + static_cast<std::ptrdiff_t>(o - 4u));
                        milo::putStr(out, og);
                        copied = end;
                    }
                }
                o = end;
            }
            out.insert(out.end(), body.begin() + static_cast<std::ptrdiff_t>(copied), body.end());
            return out;
        }

        // A proxy inlined in the stream (the crowd's lighter_flame) carries
        // its scene's objects, markers and all, ahead of the next object, so
        // split at the markers the scene runs into the entries after it.
        // Those are the file's last bodies, one a marker counted from its end;
        // the proxy's own body is all before them.
        milo::Dir unInline(milo::Dir dir)
        {
            static const uint8_t kMark[4] = {0xad, 0xde, 0xad, 0xde};
            for (size_t i = 0; i < dir.entries.size(); ++i)
            {
                if (!inlinedProxy(dir.entries[i].first, dir.bodies[i]))
                    continue;
                Bytes rest;
                for (size_t j = i; j < dir.bodies.size(); ++j)
                {
                    rest.insert(rest.end(), dir.bodies[j].begin(), dir.bodies[j].end());
                    rest.insert(rest.end(), kMark, kMark + 4);
                }
                std::vector<size_t> ends;
                for (auto at = rest.begin(); (at = std::search(at, rest.end(), kMark, kMark + 4)) != rest.end(); at += 4)
                    ends.push_back(static_cast<size_t>(at - rest.begin()) + 4u);
                const size_t bodies = dir.bodies.size() - i;
                if (ends.size() < bodies)
                    continue;
                size_t start = 0u;
                for (size_t k = 0; k < bodies; ++k)
                {
                    const size_t end = ends[ends.size() - bodies + k];
                    dir.bodies[i + k].assign(rest.begin() + static_cast<std::ptrdiff_t>(start),
                                             rest.begin() + static_cast<std::ptrdiff_t>(end - 4u));
                    start = end;
                }
            }
            return dir;
        }

        bool names(const Bytes &body, const std::string &name)
        {
            Bytes word;
            milo::putStr(word, name);
            return std::search(body.begin(), body.end(), word.begin(), word.end()) != body.end();
        }
    }

    std::string ps2Path(const std::string &xboxPath)
    {
        std::string path = ogFolders(xboxPath);
        if (path.ends_with(".milo_xbox"))
            path.replace(path.size() - 4u, 4u, "ps2");
        return path;
    }

    // With ownRoot the 360 scene's own object has to convert; without, the
    // PS2's stands where it does not (the track's is a GHTrackDir).
    static std::optional<milo::Dir> convertWith(const SceneSource &source, bool ownRoot)
    {
        const milo::Dir xbox = unInline(source.xbox);
        const milo::Dir &ps2 = source.ps2;
        std::map<std::string, std::pair<std::string, const Bytes *>> twin;
        for (size_t i = 0; i < ps2.entries.size(); ++i)
            twin[ps2.entries[i].second] = {ps2.entries[i].first, &ps2.bodies[i]};

        std::string hud;
        if (xbox.className == "WorldDir" && ps2.className == "WorldDir")
            hud = worldHud(ps2.root).value_or("");
        const bool worldFx = xbox.className == "RndDir" && ps2.className == "WorldFx";
        auto root = worldFx ? worldFxRoot(xbox.root, ps2.root) : rootToPs2(xbox.className, xbox.root, hud);
        if (!root && ownRoot)
            return std::nullopt;
        if (!root)
            root = ps2.root;

        milo::Dir out;
        out.version = 24u;
        out.className = worldFx ? ps2.className : xbox.className;
        out.name = ps2.name;
        std::set<std::string> xboxNames;
        for (const auto &entry : xbox.entries)
            xboxNames.insert(entry.second);
        auto has = [&](const std::string &name)
        {
            return std::any_of(out.entries.begin(), out.entries.end(), [&](const auto &e) { return e.second == name; });
        };
        auto keep = [&](const std::string &className, const std::string &name, Bytes body)
        {
            out.entries.emplace_back(className, name);
            out.bodies.push_back(std::move(body));
        };

        std::map<std::string, int> dropped;
        std::vector<size_t> texes;
        std::set<std::string> maps;
        for (size_t i = 0; i < xbox.entries.size(); ++i)
        {
            const auto &[className, name] = xbox.entries[i];
            const Bytes &body = xbox.bodies[i];
            const auto own = twin.find(name);
            std::optional<Bytes> made;
            if (className == "Tex")
            {
                texes.push_back(i);
                continue;
            }
            if (fromTwin(className) || (className == "RndDir" && own != twin.end()))
            {
                if (own != twin.end())
                    keep(own->second.first, name, *own->second.second);
                continue;
            }
            if (className == "Mat")
            {
                if (auto mat = matToPs2(body))
                {
                    if (source.look)
                        source.look(name, *mat);
                    for (std::string &map : mat->maps)
                        if (!map.empty())
                            maps.insert(std::move(map));
                    made = std::move(mat->body);
                }
            }
            else if (className == "Spotlight")
            {
                // A Group around the light can, which the PS2 casts its name to.
                const auto can = spotlightCan(body);
                std::string group;
                if (can && !can->empty())
                {
                    group = can->substr(0u, can->rfind('.')) + "_can.grp";
                    if (!has(group) && !xboxNames.contains(group))
                        keep("Group", group, canGroup(*can));
                }
                made = can ? spotlightToPs2(body, group) : std::nullopt;
            }
            else if (!noPs2Class(className))
                made = toPs2(className, body);
            if (made && className == "ParticleSys" && source.spin)
                if (const auto spin = particleSpin(body))
                    source.spin(name, *spin);
            if (made && className == "CamShot")
                if (auto focus = camShotFocus(body))
                    keep(className, focusShot(name), std::move(*focus));
            if (made)
                keep(className, name, std::move(*made));
            else if (!noPs2Class(className))
                ++dropped[className];
        }

        // Plain Objects are script hooks (grim's expression_task, named in
        // the PS2's scripts), which the 360 has none of.
        for (size_t i = 0; i < ps2.entries.size(); ++i)
            if (ps2.entries[i].first == "Object" && !has(ps2.entries[i].second))
                keep("Object", ps2.entries[i].second, ps2.bodies[i]);

        // The pictures, but those only a normal, specular, glow, ramp or
        // environ map names: the PS2 draws none of those.
        // The bodies so far, counted: the list grows as Texes are kept.
        const size_t held = out.bodies.size();
        const auto named = [&](const std::string &name)
        {
            return names(*root, name) ||
                   std::any_of(out.bodies.begin(), out.bodies.begin() + static_cast<std::ptrdiff_t>(held),
                               [&](const Bytes &body) { return names(body, name); });
        };
        std::vector<std::pair<size_t, Tex>> pictures;
        for (const size_t i : texes)
        {
            const std::string &name = xbox.entries[i].second;
            const auto tex = readTex(xbox.bodies[i]);
            if (!tex)
                ++dropped["Tex"];
            else if (!tex->picture)
            {
                if (auto made = texToPs2(xbox.bodies[i], {}))
                    keep("Tex", name, std::move(*made));
            }
            else if (!maps.contains(name) || named(name))
                pictures.emplace_back(i, *tex);
        }
        for (const auto &[i, tex] : pictures)
        {
            const std::string &name = xbox.entries[i].second;
            const Bytes standIn = source.standIn(name, tex.width, tex.height);
            if (auto made = texToPs2(xbox.bodies[i], standIn))
                keep("Tex", name, std::move(*made));
            else
                ++dropped["Tex"];
        }

        const std::string base = source.path.substr(0u, source.path.rfind('/'));
        out.root = fixPaths(source, base, *root);
        for (Bytes &body : out.bodies)
            body = fixPaths(source, base, body);

        // The string table hints: what the 360's held beyond its entries,
        // and room for the PS2's name.
        uint32_t size = static_cast<uint32_t>(out.name.size()) + 64u;
        for (const auto &[className, name] : out.entries)
            size += static_cast<uint32_t>(className.size() + name.size() + 2u);
        const int64_t extra = int64_t{xbox.tableCount} - 2 * static_cast<int64_t>(xbox.entries.size());
        out.tableCount = static_cast<uint32_t>(2u * out.entries.size() + static_cast<size_t>(std::max<int64_t>(2, extra)));
        out.tableSize = std::max(xbox.tableSize, size);

        for (const auto &[className, count] : dropped)
            std::cerr << "[gh2x] " << source.path << ": " << count << " " << className << " left out" << std::endl;
        return out;
    }

    std::optional<milo::Dir> convertScene(const SceneSource &source) { return convertWith(source, true); }

    namespace
    {
        constexpr const char *kSwing = "wood_track_surface_starpower.tnm";
        constexpr const char *kSwingFilter = "wood_track_surface.filt";
        constexpr const char *kLightning = "lightning_track.view";
        constexpr const char *kLightningFirst = "lightning_1.mnm";
        constexpr const char *kStarBase = "star_base.mat";

        // Where a PS2 Mat's body has its z mode: past its revisions, type,
        // note, blend, colour and two flags.
        std::optional<size_t> zModeAt(const milo::Bytes &body)
        {
            const auto word = [&](size_t at) -> std::optional<uint32_t>
            {
                if (at + 4u > body.size())
                    return std::nullopt;
                uint32_t v;
                std::memcpy(&v, body.data() + at, 4u);
                return v;
            };
            const auto objectRevision = word(4u), typeLength = word(8u);
            if (!objectRevision || !typeLength)
                return std::nullopt;
            size_t at = 12u + *typeLength + 1u;
            if (*objectRevision >= 1u)
            {
                const auto noteLength = word(at);
                if (!noteLength)
                    return std::nullopt;
                at += 4u + *noteLength;
            }
            at += 4u + 16u + 2u;
            return at + 4u <= body.size() ? std::optional<size_t>(at) : std::nullopt;
        }
    }

    std::optional<milo::Dir> convertTrack(const SceneSource &source)
    {
        const auto converted = convertWith(source, false);
        if (!converted)
            return std::nullopt;
        static const std::set<std::string> kTaken = {"Mat", "Tex", "Light", "Environ"};
        milo::Dir out = source.ps2;
        std::map<std::string, size_t> at;
        for (size_t i = 0; i < out.entries.size(); ++i)
            at[out.entries[i].second] = i;
        for (size_t i = 0; i < converted->entries.size(); ++i)
        {
            const auto &[className, name] = converted->entries[i];
            if (!kTaken.contains(className))
                continue;
            const auto found = at.find(name);
            if (found == at.end())
                milo::add(out, className, name, converted->bodies[i]);
            else if (out.entries[found->second].first == className)
            {
                // The 360 draws a star gem's base first and untested, the
                // gem and its black top over it (its TrackWidgets' order).
                // The PS2's track draws the base after them, so it keeps the
                // PS2 Mat's z mode, which leaves it under them.
                milo::Bytes body = converted->bodies[i];
                const auto from = zModeAt(out.bodies[found->second]), to = zModeAt(body);
                if (name == kStarBase && from && to)
                    std::memcpy(body.data() + *to, out.bodies[found->second].data() + *from, 4u);
                out.bodies[found->second] = std::move(body);
            }
        }
        // The 360 swings the board's second light in star power
        // (wood_track_surface.filt over wood_track_surface_starpower.tnm,
        // in its lightning.grp). The PS2's track animates its
        // lightning_track.view then (Track::SetupLightning 0x150bb0), so the
        // filter goes first in that group's list, found by its first name.
        const auto swing = milo::find(*converted, kSwing), filter = milo::find(*converted, kSwingFilter);
        const auto group = milo::find(out, kLightning);
        if (swing && filter && group && !at.contains(kSwing) && !at.contains(kSwingFilter))
        {
            milo::Bytes first, added;
            milo::putStr(first, kLightningFirst);
            milo::putStr(added, kSwingFilter);
            milo::Bytes &body = out.bodies[*group];
            const auto list = std::search(body.begin(), body.end(), first.begin(), first.end());
            if (list != body.end() && list - body.begin() >= 4)
            {
                uint32_t count;
                std::memcpy(&count, &*(list - 4), 4u);
                ++count;
                std::memcpy(&*(list - 4), &count, 4u);
                body.insert(list, added.begin(), added.end());
                for (const auto &[className, name, from] :
                     {std::tuple{"TransAnim", kSwing, *swing}, std::tuple{"AnimFilter", kSwingFilter, *filter}})
                    milo::add(out, className, name, converted->bodies[from]);
            }
        }
        return out;
    }
}
