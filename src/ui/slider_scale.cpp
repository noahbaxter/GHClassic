#include "ui/slider_scale.h"

#include "hook.h"
#include "render/texture_capture.h"
#include "script.h"

#include <algorithm>
#include <map>
#include <memory>

namespace gh2::slider_scale
{
    namespace
    {
        // A plain slider's Hmx::Object, and the colour its scale was last
        // drawn in.
        struct Colour
        {
            float rgb[3] = {1.0f, 1.0f, 1.0f};
        };
        std::map<uint32_t, Colour> s_plain;
        std::shared_ptr<const TextureData> s_scale; // made at the first draw

        struct DrawTag;

        // A BandSlider's Hmx::Object, which script names it by, from the
        // slider itself.
        constexpr uint32_t kObject = 0x160u;

        // BandSlider::DrawShowing (retail 0x296bd8) has the slider's meshes
        // drawn, the scale among them, and they are drawn after it has left
        // (it ends in a jump). So only its entry is ours: from here to the
        // next slider's, the scale captured is this one's.
        void onDraw(uint8_t *, R5900Context *ctx, PS2Runtime *)
        {
            const auto found = s_plain.find(GPR_U32(ctx, 4) + kObject);
            if (found == s_plain.end())
                return drawSliderScalesAs(nullptr);
            if (!s_scale)
                s_scale = plainSliderScale();
            Colour *colour = &found->second; // a map's values stay where they are
            drawSliderScalesAs(s_scale, [colour](const float *rgb) { std::copy(rgb, rgb + 3, colour->rgb); });
        }

        script::Node command(const script::Call &call)
        {
            const std::string op = call.symbol(1);
            if (op == "clear")
            {
                s_plain.clear();
                drawSliderScalesAs(nullptr);
            }
            else if (op == "plain")
                s_plain[call.object(2)];
            else if (op == "colour")
            {
                // As {pack_color r g b} makes one.
                const auto found = s_plain.find(call.object(2));
                if (found == s_plain.end())
                    return {};
                uint32_t packed = 0u;
                for (int i = 0; i < 3; ++i)
                    packed |= static_cast<uint32_t>(std::clamp(found->second.rgb[i], 0.0f, 1.0f) * 255.0f + 0.5f) << (8 * i);
                return {packed, script::kInt};
            }
            return {};
        }
    }

    void install(PS2Runtime &runtime, const Addresses &addresses)
    {
        EntryHook<DrawTag>::install(runtime, addresses.bandSliderDrawShowing, onDraw);
        script::addCommand("slider_scales", command);
    }
}
