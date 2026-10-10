#pragma once

// A 360 scene file as the PS2 engine loads it, in place of the PS2 disc's
// file at the same path (gh2x/models.h). Each object goes through its class's
// rule (gh2x/classes.h); the PS2 file stands in for what has no 360 form the
// PS2 can take, and the pictures stay on the host behind stand-ins
// (render/texture_store.h).

#include "gh2x/classes.h"
#include "milo/milo.h"

#include <functional>
#include <optional>
#include <string>

namespace gh2::gh2x
{
    struct SceneSource
    {
        const milo::Dir &xbox;   // the 360's scene, version 25
        const milo::Dir &ps2;    // the PS2's at the same path
        std::string path;        // the PS2's, in its archive
        // Whether the game can have a file: the PS2 disc's, or a scene only
        // the 360 has, converted.
        std::function<bool(const std::string &path)> has;
        // A stand-in bitmap for the 360 scene's Tex of that name, whose
        // picture is that size. Empty leaves the Tex with no picture.
        std::function<milo::Bytes(const std::string &tex, uint32_t width, uint32_t height)> standIn;
        // Told of each Mat kept, with the 360's shading of it that its PS2
        // body has no use for. May be empty.
        std::function<void(const std::string &mat, const Mat &shading)> look;
        // Told of each ParticleSys whose particles turn. May be empty.
        std::function<void(const std::string &system, const Spin &spin)> spin;
    };

    // None when the scene's own object cannot be converted. Any other
    // object that cannot be is left out.
    std::optional<milo::Dir> convertScene(const SceneSource &source);

    // The PS2's track with the 360's look: the PS2's scene whole, which its
    // track code drives by name (groups, animations, the gems' meshes), with
    // each Mat, Tex, Light and Environ the 360's track has under the same
    // name in its place, and those only the 360's has beside them. The 360
    // draws its gems and lines through TrackWidgets, which the PS2 has no
    // class for.
    std::optional<milo::Dir> convertTrack(const SceneSource &source);

    // Where the PS2 disc keeps the scene at a 360 path: its ng (new
    // generation) folders are the PS2's og, x360 world/battle/ng/gen/
    // battle_geom.milo_xbox the PS2's world/battle/og/gen/battle_geom.milo_ps2.
    std::string ps2Path(const std::string &xboxPath);
}
