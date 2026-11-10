#include "render/draw_hue.h"

#include "render/hsv.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <memory>
#include <utility>

namespace gh2
{
    namespace
    {
        float s_hue = 0.0f, s_across = 1.0f, s_up = 1.0f;
        // Warm hues, in sixths of the wheel: from magenta-red round to
        // yellow-green.
        constexpr float kWarmFrom = 5.5f, kWarmUpTo = 1.4f;

        void turn(float *rgb, float degrees)
        {
            const Hsv hsv = toHsv(rgb);
            // Fire's colours alone, red to yellow and a little either side.
            if (hsv.s == 0.0f || (hsv.sixths > kWarmUpTo && hsv.sixths < kWarmFrom))
                return;
            fromHsv(std::fmod(hsv.sixths + degrees / 60.0f + 12.0f, 6.0f), hsv.s, hsv.v, rgb);
        }

        // Each texture turned once for each hue it is drawn at.
        std::shared_ptr<const TextureData> turned(const std::shared_ptr<const TextureData> &texture, float degrees)
        {
            struct Turned
            {
                std::shared_ptr<const TextureData> from, to;
            };
            static std::map<std::pair<const TextureData *, float>, Turned> s_turned;
            Turned &found = s_turned[{texture.get(), degrees}];
            if (found.from != texture)
            {
                // Those no one draws from any more go, an old track's fire.
                std::erase_if(s_turned, [&](const auto &entry)
                              { return &entry.second != &found && entry.second.from.use_count() == 1; });
                auto to = std::make_shared<TextureData>(*texture);
                const auto turnAll = [&](std::vector<uint8_t> &rgba)
                {
                    for (size_t i = 0; i + 3u < rgba.size(); i += 4u)
                    {
                        float rgb[3] = {rgba[i] / 255.0f, rgba[i + 1] / 255.0f, rgba[i + 2] / 255.0f};
                        turn(rgb, degrees);
                        for (int c = 0; c < 3; ++c)
                            rgba[i + c] = static_cast<uint8_t>(std::lround(rgb[c] * 255.0f));
                    }
                };
                turnAll(to->rgba);
                for (auto &mip : to->mips)
                    turnAll(mip);
                found = {texture, std::move(to)};
            }
            return found.to;
        }
    }

    void setDrawHue(float degrees)
    {
        s_hue = degrees;
    }
    void turnColor(float *rgb)
    {
        if (s_hue != 0.0f)
            turn(rgb, s_hue);
    }

    void turnMaterial(Material &material)
    {
        if (s_hue == 0.0f)
            return;
        turn(material.color, s_hue);
        if (material.texture)
            material.texture = turned(material.texture, s_hue);
    }

    void setSpriteStretch(float across, float up)
    {
        s_across = across;
        s_up = up;
    }
    float spriteAcross() { return s_across; }
    float spriteUp() { return s_up; }
}
