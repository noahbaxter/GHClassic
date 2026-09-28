#pragma once

#include "ps2_runtime.h"

#include <memory>

namespace gh2
{
    // The host window and present loop: SDL3 for the window, Vulkan for the
    // picture. Replaces the runtime's raylib frontend.
    class VulkanFrontend final : public PS2Runtime::HostFrontend
    {
    public:
        VulkanFrontend();
        ~VulkanFrontend() override;

        bool initialize(PS2Runtime &runtime, const char *title) override;
        bool frame(PS2Runtime &runtime) override;
        void shutdown(PS2Runtime &runtime) override;

    private:
        struct State;
        std::unique_ptr<State> m_state;
    };
}
