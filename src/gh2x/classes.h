#pragma once

// One 360 object body as the PS2 engine loads it, class by class. The 360
// writes most classes at a later revision; the fields it added are dropped
// where the PS2 has no place for them, and the few the PS2 has and the 360
// lost are filled. Layouts from the retail PS2 loaders, RB3's revision gates
// and every object on both discs (.planning/gh2x-look/DESIGN.md, "What the
// converter has to write").

#include "milo/milo.h"

#include <optional>
#include <string>
#include <vector>

namespace gh2::gh2x
{
    using milo::Bytes;

    // Classes the PS2 engine has no counterpart of, which a scene loses.
    bool noPs2Class(const std::string &className);

    // Classes whose PS2 object stands in for the 360's, of the same name in
    // the PS2's own scene: sample data in the console's own audio format, an
    // outfit list of the PS2's own folder.
    bool fromTwin(const std::string &className);

    // A body of any class but Tex, Mat, Spotlight and those above. None when
    // the class has no rule or the body does not fit its layout.
    std::optional<Bytes> toPs2(const std::string &className, const Bytes &body);

    // A Mat with its normal, emissive, specular, diffuse ramp and environ
    // maps emptied, which the PS2's has no use for, and their names in that
    // order. With them its diffuse texture's name and the fields only the
    // 360's shading reads, which the PS2 body keeps: glow multiplier,
    // specular colour and power, per pixel lighting, and normalize (its TBN
    // in the pixel shader).
    struct Mat
    {
        Bytes body;
        std::vector<std::string> maps;
        std::string diffuse;
        float emissive = 1.0f;
        float specular[4] = {};
        bool perPixel = false;
        bool normalize = false;
    };
    std::optional<Mat> matToPs2(const Bytes &body);

    // Tex 10 on both. A regular one on the 360 holds its picture block
    // compressed, which the PS2 cannot draw: picture says so, with its size.
    // None when the body does not fit the layout.
    struct Tex
    {
        bool picture = false;
        uint32_t width = 0u;
        uint32_t height = 0u;
    };
    std::optional<Tex> readTex(const Bytes &body);

    // The Tex with no picture of its own, or with `bitmap` in its place
    // (a PS2 bitmap: formats/bitmap.h), its size and depth set to match.
    std::optional<Bytes> texToPs2(const Bytes &body, const Bytes &bitmap);

    // A Spotlight's light can: a Mesh on the 360, a Group on the PS2, which
    // casts the name to one. canGroup is a Group around it at the identity,
    // for the scene to add under the name spotlightToPs2 is given.
    std::optional<std::string> spotlightCan(const Bytes &body);
    std::optional<Bytes> spotlightToPs2(const Bytes &body, const std::string &canGroup);
    Bytes canGroup(const std::string &mesh);

    // A scene's own object, by its class: a directory (WorldDir keeping the
    // PS2 scene's HUD path, hud), or a CharClipSet. None for a class neither
    // is, or one whose proxy is inlined in the stream.
    std::optional<Bytes> rootToPs2(const std::string &className, const Bytes &root, const std::string &hud);

    // Whether a 360 directory object is a proxy whose scene is inlined after
    // it in the stream, ahead of the next object (the crowd's lighter_flame).
    bool inlinedProxy(const std::string &className, const Bytes &body);

    // A 360 effect scene's RndDir root as the PS2's WorldFx, the class its
    // scene has where the 360 marks the RndDir with the type world_fx: a
    // revision, then the RndDir, typed as the PS2's (ps2Root) is.
    std::optional<Bytes> worldFxRoot(const Bytes &root, const Bytes &ps2Root);

    // A PS2 WorldDir's HUD scene path.
    std::optional<std::string> worldHud(const Bytes &root);
}
