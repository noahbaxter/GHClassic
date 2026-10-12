#include "ui/slider_scale.h"

#include "hook.h"
#include "render/texture_capture.h"
#include "script.h"

#include <algorithm>
#include <array>
#include <map>
#include <memory>
#include <optional>

namespace gh2::slider_scale
{
    namespace
    {
        using Rgb = std::array<float, 3>;

        // A plain slider's Hmx::Object, the colour its scale was last drawn
        // in, and the colour it is drawn in while it is off.
        struct Slider
        {
            float rgb[3] = {1.0f, 1.0f, 1.0f};
            std::optional<Rgb> off;
        };
        std::map<uint32_t, Slider> s_plain;
        std::map<uint32_t, Rgb> s_offBoxes; // by a CheckBox's Hmx::Object
        std::shared_ptr<const TextureData> s_scale; // made at the first draw

        struct DrawTag;
        struct BoxTag;

        // A BandSlider's Hmx::Object, which script names it by, from the
        // slider itself.
        constexpr uint32_t kObject = 0x160u;
        // And a CheckBox's, from the box.
        constexpr uint32_t kBoxObject = 0x150u;

        // BandSlider::DrawShowing (retail 0x296bd8) has the slider's meshes
        // drawn, the scale among them, and they are drawn after it has left
        // (it ends in a jump). So only its entry is ours: from here to the
        // next slider's, the scale captured is this one's.
        void onDraw(uint8_t *, R5900Context *ctx, PS2Runtime *)
        {
            const auto found = s_plain.find(GPR_U32(ctx, 4) + kObject);
            if (found == s_plain.end())
            {
                drawOff(Off::kNone);
                return drawSliderScalesAs(nullptr);
            }
            if (!s_scale)
                s_scale = plainSliderScale();
            Slider *slider = &found->second; // a map's values stay where they are
            drawOff(Off::kSlider, slider->off ? slider->off->data() : nullptr);
            drawSliderScalesAs(s_scale, [slider](const float *rgb) { std::copy(rgb, rgb + 3, slider->rgb); });
        }

        // CheckBox::DrawShowing (retail 0x29bde8): its one mesh follows.
        void onBox(uint8_t *, R5900Context *ctx, PS2Runtime *)
        {
            const auto found = s_offBoxes.find(GPR_U32(ctx, 4) + kBoxObject);
            drawOff(Off::kOne, found != s_offBoxes.end() ? found->second.data() : nullptr);
        }

        // Each is what it was told to be for as long as it is there: a
        // screen draws its sliders while it leaves, and what takes one's
        // place in memory is another, the volume's again. ~BandSlider
        // (retail 0x296528) and ~CheckBox (0x29b9b8).
        struct SliderGoneTag;
        struct BoxGoneTag;
        void onSliderGone(uint8_t *, R5900Context *ctx, PS2Runtime *) { s_plain.erase(GPR_U32(ctx, 4) + kObject); }
        void onBoxGone(uint8_t *, R5900Context *ctx, PS2Runtime *) { s_offBoxes.erase(GPR_U32(ctx, 4) + kBoxObject); }

        script::Node command(const script::Call &call)
        {
            const std::string op = call.symbol(1);
            if (op == "plain")
                s_plain[call.object(2)];
            else if (op == "off" || op == "on")
            {
                // A plain slider, or else a tick box. The colour is as
                // {pack_color r g b} makes one.
                const uint32_t object = call.object(2);
                std::optional<Rgb> colour;
                if (op == "off")
                {
                    const uint32_t packed = call.arg(3).value;
                    colour = Rgb{};
                    for (int i = 0; i < 3; ++i)
                        (*colour)[i] = static_cast<float>((packed >> (8 * i)) & 0xffu) / 255.0f;
                }
                if (const auto found = s_plain.find(object); found != s_plain.end())
                    found->second.off = colour;
                else if (colour)
                    s_offBoxes[object] = *colour;
                else
                    s_offBoxes.erase(object);
            }
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
        EntryHook<BoxTag>::install(runtime, addresses.checkBoxDrawShowing, onBox);
        EntryHook<SliderGoneTag>::install(runtime, addresses.bandSliderDtor, onSliderGone);
        EntryHook<BoxGoneTag>::install(runtime, addresses.checkBoxDtor, onBoxGone);
        script::addCommand("slider_scales", command);
    }
}
