#pragma once

#include "render/frame.h"

#include <vulkan/vulkan.h>

#include <memory>

typedef struct VmaAllocator_T *VmaAllocator;

namespace gh2
{
    // Draws a Frame's recorded draws into the guest-resolution target.
    class SceneRenderer
    {
    public:
        SceneRenderer();
        ~SceneRenderer();

        // `samples` per pixel in every pass, resolved before anything reads
        // the result.
        bool initialize(VkDevice device, VmaAllocator allocator, VkSampleCountFlagBits samples);
        void shutdown();

        // Renders into `target` (R8G8B8A8, width x height), which ends in
        // TRANSFER_SRC_OPTIMAL. `serial` counts host frames; a GPU resource is
        // freed once no frame in flight can still use it.
        bool record(VkCommandBuffer cmd, const Frame &frame, VkImage target, uint32_t width, uint32_t height,
                    uint64_t serial);

    private:
        struct State;
        std::unique_ptr<State> m_state;
    };
}
