#pragma once

#include "ps2_runtime.h"

#include <memory>

namespace gh2
{
    // What the scene is drawn at: the size the window shows it at (the
    // game's own when hidden), the game's own (512x448 as GH2 sets it), or a
    // given height at the picture's aspect, 1920x1080 for 1080 in 16:9.
    struct RenderSize
    {
        enum Mode
        {
            kWindow,
            kNative,
            kHeight,
        };
        Mode mode = kWindow;
        uint32_t height = 0; // kHeight's
    };

    // The host window and present loop: SDL3 for the window, Vulkan for the
    // picture. Replaces the runtime's raylib frontend.
    class VulkanFrontend final : public PS2Runtime::HostFrontend
    {
    public:
        explicit VulkanFrontend(RenderSize renderSize = {});
        ~VulkanFrontend() override;

        bool initialize(PS2Runtime &runtime, const char *title) override;
        bool frame(PS2Runtime &runtime) override;
        void shutdown(PS2Runtime &runtime) override;

    private:
        struct State;
        std::unique_ptr<State> m_state;
        RenderSize m_renderSize;
    };
}
