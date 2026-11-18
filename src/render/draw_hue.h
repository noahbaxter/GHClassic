#pragma once

#include "render/scene.h"

namespace gh2
{
    // What is drawn until it is set back to 0, particles and meshes, with
    // its warm colours (fire's, red to yellow), and its textures', turned
    // round the colour wheel by this many degrees. Cool ones stay.
    void setDrawHue(float degrees);

    // An rgb turned by the hue set, if warm, its saturation and value kept.
    void turnColor(float *rgb);
    // A material's colour and texture turned by the hue set.
    void turnMaterial(Material &material);

    // Particles drawn until it is set back to 1, 1: each sprite this many
    // times as wide (along the screen) and as tall.
    void setSpriteStretch(float across, float up);
    float spriteAcross();
    float spriteUp();
}
