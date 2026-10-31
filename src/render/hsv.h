#pragma once

#include <algorithm>
#include <cmath>

namespace gh2
{
    // An rgb (0 to 1) as its hue (round the wheel in sixths), saturation
    // and value, its hue 0 for a grey; and an rgb of those.
    struct Hsv
    {
        float sixths, s, v;
    };

    inline Hsv toHsv(const float *rgb)
    {
        const float v = std::max({rgb[0], rgb[1], rgb[2]}), lo = std::min({rgb[0], rgb[1], rgb[2]});
        Hsv out{0.0f, v > 0.0f ? (v - lo) / v : 0.0f, v};
        if (v > lo)
        {
            const float d = v - lo;
            out.sixths = v == rgb[0] ? std::fmod((rgb[1] - rgb[2]) / d + 6.0f, 6.0f)
                         : v == rgb[1] ? (rgb[2] - rgb[0]) / d + 2.0f
                                       : (rgb[0] - rgb[1]) / d + 4.0f;
        }
        return out;
    }

    inline void fromHsv(float sixths, float s, float v, float *rgb)
    {
        const float f = sixths - std::floor(sixths), p = v * (1.0f - s), q = v * (1.0f - s * f),
                    t = v * (1.0f - s * (1.0f - f));
        const float rows[6][3] = {{v, t, p}, {q, v, p}, {p, v, t}, {p, q, v}, {t, p, v}, {v, p, q}};
        for (int c = 0; c < 3; ++c)
            rgb[c] = rows[static_cast<int>(sixths) % 6][c];
    }
}
