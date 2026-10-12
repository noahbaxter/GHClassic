#pragma once

#include "render/frame.h"

#include <volk.h>

#include <cstdint>
#include <iterator>
#include <memory>

typedef struct VmaAllocator_T *VmaAllocator;

namespace gh2
{
    // What the GPU's time on a frame is split into, in the order it is
    // spent: the shadow maps, the rendered textures, the world up to the
    // post stage, its samples made one a pixel, the rest of the post stage,
    // what is drawn over it (the track and HUD), and the gamma ramp.
    constexpr const char *kGpuSpans[] = {"shadows", "targets", "world", "resolve", "post", "hud", "output"};

    // What the last record did, for the frame log.
    struct SceneStats
    {
        uint32_t meshesCreated = 0;
        uint32_t texturesCreated = 0;
        uint64_t uploadBytes = 0;
        uint32_t evicted = 0;
        // The GPU's milliseconds a span on the newest frame it has finished,
        // a few behind the one recorded. Zero where the frame log is off or
        // the device keeps no times.
        float gpuMs[std::size(kGpuSpans)]{};
    };

    // Draws a Frame's recorded draws into a target image, at the size given.
    class SceneRenderer
    {
    public:
        SceneRenderer();
        ~SceneRenderer();

        // `samples` per pixel in every pass, resolved before anything reads
        // the result. `everySample`: the device shades each of them apart
        // (sampleRateShading), which the 360's surfaces are then drawn with.
        bool initialize(VkDevice device, VmaAllocator allocator, VkSampleCountFlagBits samples, bool everySample);
        void shutdown();

        // Renders into `target` (R8G8B8A8, width x height), which ends in
        // TRANSFER_SRC_OPTIMAL. `serial` counts host frames; a GPU resource is
        // freed once no frame in flight can still use it.
        bool record(VkCommandBuffer cmd, const Frame &frame, VkImage target, uint32_t width, uint32_t height,
                    uint64_t serial);

        // The counts from the last record (and the retiring after it).
        [[nodiscard]] SceneStats stats() const;

    private:
        // With the 360's shading and post, or the PS2's.
        bool create(VkDevice device, VmaAllocator allocator, VkSampleCountFlagBits samples, bool everySample, bool x360);

        struct State;
        std::unique_ptr<State> m_state;
    };
}
