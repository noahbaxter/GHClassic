#include "host/vulkan_frontend.h"

#include "dev/cheats.h"
#include "host/audio.h"
#include "dev/draw_dump.h"
#include "dev/timing.h"
#include "host/input.h"
#include "host/perf.h"
#include "host/record.h"
#include "host/scene_renderer.h"
#include "movie/screen.h"
#include "render/frame.h"
#include "render/texture_store.h"
#include "runtime/ee_scheduler.h"
#include "runtime/host_clock.h"
#include "settings/settings.h"
#include "settings/video_options.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <VkBootstrap.h>
#include <vk_mem_alloc.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace gh2
{
    namespace
    {
        std::mutex s_shotMutex;
        std::string s_shotRequest;
        bool s_shotWriting = false; // taken from the request, not yet on disk

        constexpr uint32_t kFramesInFlight = 2;
        // The window opens at the picture's shape: 16:9 with widescreen, else 4:3.
        constexpr int kWindowHeight = 720;
        constexpr int kWindowWidthWide = 1280;
        constexpr int kWindowWidth = 960;

        bool check(VkResult result, const char *what)
        {
            if (result == VK_SUCCESS)
                return true;
            std::cerr << "[vulkan] " << what << " failed: " << result << std::endl;
            return false;
        }

        void imageBarrier(VkCommandBuffer cmd, VkImage image, VkImageLayout from, VkImageLayout to,
                          VkAccessFlags srcAccess, VkAccessFlags dstAccess,
                          VkPipelineStageFlags srcStage, VkPipelineStageFlags dstStage)
        {
            VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
            barrier.srcAccessMask = srcAccess;
            barrier.dstAccessMask = dstAccess;
            barrier.oldLayout = from;
            barrier.newLayout = to;
            barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.image = image;
            barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            vkCmdPipelineBarrier(cmd, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &barrier);
        }
    }

    struct VulkanFrontend::State
    {
        bool hidden = false;
        std::filesystem::path shotDir;
        uint32_t shotEvery = 0;
        uint64_t presented = 0;

        SDL_Window *window = nullptr;
        bool mouseKept = false;
        vkb::Instance instance;
        VkSurfaceKHR surface = VK_NULL_HANDLE;
        vkb::Device device;
        VkQueue queue = VK_NULL_HANDLE;
        uint32_t queueFamily = 0;
        VmaAllocator allocator = VK_NULL_HANDLE;

        vkb::Swapchain swapchain;
        std::vector<VkImage> swapchainImages;
        std::vector<VkSemaphore> renderFinished; // one per swapchain image
        bool swapchainStale = false;

        // The picture: at the window's size when shown, else the guest's.
        VkImage target = VK_NULL_HANDLE;
        VmaAllocation targetMemory = VK_NULL_HANDLE;
        uint32_t targetWidth = 0;
        uint32_t targetHeight = 0;

        VkCommandPool commandPool = VK_NULL_HANDLE;
        struct Slot
        {
            VkCommandBuffer cmd = VK_NULL_HANDLE;
            VkFence done = VK_NULL_HANDLE;
            VkSemaphore imageAvailable = VK_NULL_HANDLE;
            // A movie picture or the overlay on its way into the target.
            VkBuffer upload = VK_NULL_HANDLE;
            VmaAllocation uploadMemory = VK_NULL_HANDLE;
            void *uploadMapped = nullptr;
            VkDeviceSize uploadSize = 0;
        };
        std::array<Slot, kFramesInFlight> slots{};
        uint32_t slot = 0;

        // --timing: when each present reached the display, where the driver
        // says (VK_GOOGLE_display_timing), on its clock, which runs
        // driverClockAhead of timing::now().
        PFN_vkGetPastPresentationTimingGOOGLE pastTiming = nullptr;
        uint64_t driverClockAhead = 0;
        uint32_t presentId = 0;
        struct Present
        {
            uint32_t id = 0;
            uint64_t calledNs = 0, publishedNs = 0;
        };
        static constexpr uint32_t kPresentsKept = 64;
        std::array<Present, kPresentsKept> presents{};

        VkBuffer readback = VK_NULL_HANDLE;
        VmaAllocation readbackMemory = VK_NULL_HANDLE;
        VkDeviceSize readbackSize = 0;

        std::chrono::steady_clock::time_point nextHiddenFrame{};
        Frame shown;                                 // the game frame last drawn
        movie::Picture moviePicture;                 // the movie's, while one plays
        std::chrono::microseconds frameWait{8333};   // half a display period
        std::chrono::nanoseconds gamePeriod{16667000};

        // The game's frame rate from settings, as the runtime's vblank: one
        // game frame a vblank.
        void applyFrameRate(PS2Runtime &runtime)
        {
            float displayHz = 0.0f;
            if (!hidden)
                if (const SDL_DisplayMode *mode = SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(window)))
                    displayHz = mode->refresh_rate;
            float hz = static_cast<float>(settings::get(settings::kFrameRateCap));
            if (hz == 0.0f)
                hz = displayHz > 0.0f ? displayHz : 60.0f;
            // frame_step replays at most four PS2 frames in one of ours.
            hz = std::max(hz, 15.0f);
            gamePeriod = std::chrono::nanoseconds(static_cast<int64_t>(1e9 / hz));
            runtime.eeScheduler().setVBlankPeriod(gamePeriod);
            if (displayHz > 0.0f)
                frameWait = std::chrono::microseconds(static_cast<int64_t>(500000.0f / displayHz));
        }

        SceneRenderer scene;

        // settings.ini's [dev] frame_log: a row per presented frame in
        // frametimes.csv beside the settings, put on disk once a second, and
        // a line in the log for any frame over kSlowMs with what it did. The
        // GPU's times are of a frame a few before the row's.
        struct FrameLog
        {
            static constexpr double kSlowMs = 20.0;
            using Clock = std::chrono::steady_clock;

            bool on = false;
            std::ofstream out;
            Clock::time_point start{}, lastPresent{}, lastFlush{};
            uint64_t lastGame = 0;
            uint64_t lastDropped = 0;
            textures::Reads lastReads;

            static double ms(Clock::duration d)
            {
                return std::chrono::duration<double, std::milli>(d).count();
            }

            void open()
            {
                on = settings::get(settings::kFrameLog) != 0 || perf::logging();
                if (!on)
                    return;
                const std::string path = settings::besideSettings("frametimes.csv");
                out.open(path, std::ios::trunc);
                if (!out)
                {
                    std::cerr << "[frame] could not write " << path << std::endl;
                    on = false;
                    return;
                }
                out << "t_ms,dt_ms,new,game,wait_ms,acq_ms,rec_ms,mesh_new,tex_new,upload_kb,evicted,vbl_drop,"
                       "tex_reads,tex_read_ms,gpu_ms";
                for (const char *span : kGpuSpans)
                    out << ",gpu_" << span << "_ms";
                out << "\n";
                start = lastPresent = lastFlush = Clock::now();
                std::cerr << "[frame] logging to " << path << std::endl;
            }

            void add(bool isNew, uint64_t game, double waitMs, double acqMs, double recMs, const SceneStats &scene,
                     uint64_t dropped)
            {
                const Clock::time_point now = Clock::now();
                const double dtMs = ms(now - lastPresent);
                lastPresent = now;
                const uint64_t newGame = game - std::min(game, lastGame);
                lastGame = game;
                const uint64_t newDropped = dropped - std::min(dropped, lastDropped);
                lastDropped = dropped;
                // The game thread's texture reads since the last row.
                const textures::Reads reads = textures::reads();
                const uint64_t newReads = reads.count - lastReads.count;
                const double readMs = static_cast<double>(reads.ns - lastReads.ns) / 1e6;
                lastReads = reads;
                float gpuMs = 0.0f;
                for (float span : scene.gpuMs)
                    gpuMs += span;
                char row[192];
                std::snprintf(row, sizeof(row), "%.2f,%.2f,%d,%llu,%.2f,%.2f,%.2f,%u,%u,%llu,%u,%llu,%llu,%.2f,%.2f",
                              ms(now - start), dtMs, isNew ? 1 : 0, static_cast<unsigned long long>(newGame), waitMs,
                              acqMs, recMs, scene.meshesCreated, scene.texturesCreated,
                              static_cast<unsigned long long>(scene.uploadBytes >> 10), scene.evicted,
                              static_cast<unsigned long long>(newDropped), static_cast<unsigned long long>(newReads),
                              readMs, gpuMs);
                out << row;
                for (float span : scene.gpuMs)
                {
                    std::snprintf(row, sizeof(row), ",%.2f", span);
                    out << row;
                }
                out << "\n";
                if (dtMs > kSlowMs)
                    std::cerr << "[frame] " << static_cast<int>(dtMs) << " ms: wait " << static_cast<int>(waitMs)
                              << ", acq " << static_cast<int>(acqMs) << ", rec " << static_cast<int>(recMs)
                              << ", gpu " << static_cast<int>(gpuMs) << ", +" << scene.meshesCreated << " meshes, +"
                              << scene.texturesCreated << " tex (" << (scene.uploadBytes >> 10) << " KB), "
                              << scene.evicted << " evicted, " << newDropped << " vblanks dropped, " << newReads
                              << " game thread reads (" << static_cast<int>(readMs) << " ms)" << std::endl;
                if (now - lastFlush >= std::chrono::seconds(1))
                {
                    out.flush();
                    lastFlush = now;
                }
            }
        };
        FrameLog frameLog;
        perf::Times times;

        // The perf_overlay setting's panel into the top left corner of the
        // picture at `picture` in `image`: the window's, over the scene once
        // it is there, so render_scale leaves the panel its size, or a hidden
        // run's target, where its shots show it. As large to the eye at any
        // size the picture is.
        bool recordOverlay(VkCommandBuffer cmd, Slot &into, VkImage image, const VkRect2D &picture, bool windows)
        {
            uint32_t width = 0, height = 0;
            const uint32_t scale = std::max(1u, (picture.extent.height + 240u) / 480u);
            const std::vector<uint8_t> &pixels =
                times.draw(settings::get(settings::kPerfOverlay), scale, width, height);
            const uint32_t edge = 8u * scale;
            if (pixels.empty() || width + edge > picture.extent.width || height + edge > picture.extent.height)
                return true;
            if (!roomToUpload(into, pixels.size()))
                return false;
            uint8_t *mapped = static_cast<uint8_t *>(into.uploadMapped);
            std::memcpy(mapped, pixels.data(), pixels.size());
            // The window's is blue first.
            if (windows)
                for (size_t i = 0; i < pixels.size(); i += 4)
                    std::swap(mapped[i], mapped[i + 2]);
            vmaFlushAllocation(allocator, into.uploadMemory, 0, pixels.size());
            // The window's image is being written already.
            if (!windows)
                imageBarrier(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                             VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
            VkBufferImageCopy copy{};
            copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            copy.imageOffset = {picture.offset.x + static_cast<int32_t>(edge),
                                picture.offset.y + static_cast<int32_t>(edge), 0};
            copy.imageExtent = {width, height, 1};
            vkCmdCopyBufferToImage(cmd, into.upload, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
            if (!windows)
                imageBarrier(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                             VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
            return true;
        }

        bool createSwapchain()
        {
            int width = 0, height = 0;
            SDL_GetWindowSizeInPixels(window, &width, &height);
            vkb::SwapchainBuilder builder(device);
            // UNORM, so the guest's display-referred colours pass through as is.
            builder.set_desired_format({VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR})
                .set_desired_present_mode(timing::presentMode >= 0 ? static_cast<VkPresentModeKHR>(timing::presentMode)
                                                                   : VK_PRESENT_MODE_FIFO_KHR)
                .set_desired_extent(static_cast<uint32_t>(width), static_cast<uint32_t>(height))
                .add_image_usage_flags(VK_IMAGE_USAGE_TRANSFER_DST_BIT)
                .set_old_swapchain(swapchain);
            if (timing::swapImages > 0)
                builder.set_desired_min_image_count(static_cast<uint32_t>(timing::swapImages));
#ifdef __APPLE__
            // MoltenVK frees an image only once its last present is on the
            // display, so a full swapchain is that many refreshes from the
            // present call to the screen: 48.7 ms with three at 60 Hz, 32.0
            // with two.
            else
                builder.set_desired_min_image_count(2u);
#endif
            auto built = builder.build();
            if (!built)
            {
                std::cerr << "[vulkan] swapchain: " << built.error().message() << std::endl;
                return false;
            }
            destroySwapchain();
            swapchain = built.value();
            swapchainImages = swapchain.get_images().value();
            renderFinished.resize(swapchainImages.size());
            for (VkSemaphore &semaphore : renderFinished)
            {
                VkSemaphoreCreateInfo info{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
                if (!check(vkCreateSemaphore(device.device, &info, nullptr, &semaphore), "semaphore"))
                    return false;
            }
            swapchainStale = false;
            if (timing::on())
            {
                VkSurfaceCapabilitiesKHR caps{};
                vkGetPhysicalDeviceSurfaceCapabilitiesKHR(device.physical_device.physical_device, surface, &caps);
                uint32_t count = 0;
                vkEnumerateDeviceExtensionProperties(device.physical_device.physical_device, nullptr, &count, nullptr);
                std::vector<VkExtensionProperties> extensions(count);
                vkEnumerateDeviceExtensionProperties(device.physical_device.physical_device, nullptr, &count,
                                                     extensions.data());
                std::string timingExtensions;
                for (const VkExtensionProperties &e : extensions)
                    for (const char *wanted : {"VK_GOOGLE_display_timing", "VK_KHR_present_wait", "VK_KHR_present_id",
                                               "VK_EXT_present_timing", "VK_EXT_swapchain_maintenance1",
                                               "VK_NV_low_latency2", "VK_AMD_anti_lag"})
                        if (std::strcmp(e.extensionName, wanted) == 0)
                            timingExtensions += std::string(" ") + wanted;
                const SDL_DisplayMode *mode = SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(window));
                timing::note("swapchain " + std::to_string(swapchainImages.size()) + " images (surface min " +
                             std::to_string(caps.minImageCount) + ", max " + std::to_string(caps.maxImageCount) +
                             "), present mode " + std::to_string(swapchain.present_mode) + " (2 is FIFO), " +
                             std::to_string(swapchain.extent.width) + "x" + std::to_string(swapchain.extent.height) +
                             ", display " + std::to_string(mode ? mode->refresh_rate : 0.0f) + " Hz, video driver " +
                             SDL_GetCurrentVideoDriver() + ", fullscreen " +
                             ((SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN) ? "yes" : "no") +
                             ", timing extensions:" + (timingExtensions.empty() ? " none" : timingExtensions));
            }
            return true;
        }

        void destroySwapchain()
        {
            for (VkSemaphore semaphore : renderFinished)
                vkDestroySemaphore(device.device, semaphore, nullptr);
            renderFinished.clear();
            if (swapchain.swapchain != VK_NULL_HANDLE)
                vkb::destroy_swapchain(swapchain);
            swapchain = {};
        }

        bool ensureTarget(uint32_t width, uint32_t height)
        {
            if (target != VK_NULL_HANDLE && width == targetWidth && height == targetHeight)
                return true;
            vkDeviceWaitIdle(device.device);
            destroyTarget();

            VkImageCreateInfo image{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
            image.imageType = VK_IMAGE_TYPE_2D;
            image.format = VK_FORMAT_R8G8B8A8_UNORM;
            image.extent = {width, height, 1};
            image.mipLevels = 1;
            image.arrayLayers = 1;
            image.samples = VK_SAMPLE_COUNT_1_BIT;
            image.tiling = VK_IMAGE_TILING_OPTIMAL;
            image.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                          VK_IMAGE_USAGE_TRANSFER_DST_BIT;
            VmaAllocationCreateInfo alloc{};
            alloc.usage = VMA_MEMORY_USAGE_AUTO;
            if (!check(vmaCreateImage(allocator, &image, &alloc, &target, &targetMemory, nullptr), "target image"))
                return false;

            readbackSize = VkDeviceSize(width) * height * 4u;
            VkBufferCreateInfo buffer{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
            buffer.size = readbackSize;
            buffer.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
            VmaAllocationCreateInfo readAlloc{};
            readAlloc.usage = VMA_MEMORY_USAGE_AUTO;
            readAlloc.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT;
            if (!check(vmaCreateBuffer(allocator, &buffer, &readAlloc, &readback, &readbackMemory, nullptr),
                       "readback buffer"))
                return false;

            targetWidth = width;
            targetHeight = height;
            return true;
        }

        void destroyTarget()
        {
            if (target != VK_NULL_HANDLE)
                vmaDestroyImage(allocator, target, targetMemory);
            if (readback != VK_NULL_HANDLE)
                vmaDestroyBuffer(allocator, readback, readbackMemory);
            target = VK_NULL_HANDLE;
            readback = VK_NULL_HANDLE;
        }

        // The movie picture into the target, which is its size, left as the
        // scene leaves it.
        bool recordMovie(VkCommandBuffer cmd, Slot &into)
        {
            const VkDeviceSize size = moviePicture.rgba.size();
            if (!roomToUpload(into, size))
                return false;
            std::memcpy(into.uploadMapped, moviePicture.rgba.data(), size);
            vmaFlushAllocation(allocator, into.uploadMemory, 0, size);

            imageBarrier(cmd, target, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                         VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT);
            VkBufferImageCopy copy{};
            copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            copy.imageExtent = {targetWidth, targetHeight, 1};
            vkCmdCopyBufferToImage(cmd, into.upload, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
            imageBarrier(cmd, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                         VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT);
            return true;
        }

        // The slot's upload buffer, with room for `size`.
        bool roomToUpload(Slot &into, VkDeviceSize size)
        {
            if (into.uploadSize < size)
            {
                if (into.upload != VK_NULL_HANDLE)
                    vmaDestroyBuffer(allocator, into.upload, into.uploadMemory);
                into.upload = VK_NULL_HANDLE;
                VkBufferCreateInfo buffer{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
                buffer.size = size;
                buffer.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
                VmaAllocationCreateInfo alloc{};
                alloc.usage = VMA_MEMORY_USAGE_AUTO;
                alloc.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
                VmaAllocationInfo info{};
                if (!check(vmaCreateBuffer(allocator, &buffer, &alloc, &into.upload, &into.uploadMemory, &info),
                           "upload buffer"))
                    return false;
                into.uploadMapped = info.pMappedData;
                into.uploadSize = size;
            }
            return true;
        }

        // Where the picture sits in the window: the frame's display aspect,
        // as large as fits, centred.
        VkRect2D pictureRect(float displayAspect) const
        {
            const VkExtent2D extent = swapchain.extent;
            float width = static_cast<float>(extent.width);
            float height = width / displayAspect;
            if (height > static_cast<float>(extent.height))
            {
                height = static_cast<float>(extent.height);
                width = height * displayAspect;
            }
            const int32_t x = static_cast<int32_t>((static_cast<float>(extent.width) - width) * 0.5f);
            const int32_t y = static_cast<int32_t>((static_cast<float>(extent.height) - height) * 0.5f);
            return {{x, y}, {static_cast<uint32_t>(width), static_cast<uint32_t>(height)}};
        }

        // The picture into the window at pictureRect, and the overlay over it.
        bool blitToSwapchain(VkCommandBuffer cmd, Slot &slot, VkImage image, float displayAspect, bool overlay)
        {
            imageBarrier(cmd, image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                         VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT);
            const VkClearColorValue black{{0.0f, 0.0f, 0.0f, 1.0f}};
            const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            vkCmdClearColorImage(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &range);

            const VkRect2D rect = pictureRect(displayAspect);
            VkImageBlit blit{};
            blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            blit.srcOffsets[1] = {static_cast<int32_t>(targetWidth), static_cast<int32_t>(targetHeight), 1};
            blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            blit.dstOffsets[0] = {rect.offset.x, rect.offset.y, 0};
            blit.dstOffsets[1] = {rect.offset.x + static_cast<int32_t>(rect.extent.width),
                                  rect.offset.y + static_cast<int32_t>(rect.extent.height), 1};
            vkCmdBlitImage(cmd, target, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
            if (overlay && !recordOverlay(cmd, slot, image, rect, true))
                return false;

            imageBarrier(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                         VK_ACCESS_TRANSFER_WRITE_BIT, 0, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
            return true;
        }

        // Harness screenshots, as binary PPM; scripts/arm.sh converts them.
        // A requested one is named, the periodic ones numbered.
        void writeShot(const std::string &requested)
        {
            void *mapped = nullptr;
            if (vmaMapMemory(allocator, readbackMemory, &mapped) != VK_SUCCESS)
                return;
            vmaInvalidateAllocation(allocator, readbackMemory, 0, VK_WHOLE_SIZE);
            char numbered[32];
            std::snprintf(numbered, sizeof(numbered), "frame_%06llu.ppm", static_cast<unsigned long long>(presented));
            const std::string name = requested.empty() ? numbered : "shot_" + requested + ".ppm";
            if (FILE *file = std::fopen((shotDir / name).string().c_str(), "wb"))
            {
                std::fprintf(file, "P6\n%u %u\n255\n", targetWidth, targetHeight);
                const uint8_t *pixels = static_cast<const uint8_t *>(mapped);
                for (VkDeviceSize i = 0; i < readbackSize; i += 4)
                    std::fwrite(pixels + i, 1, 3, file);
                std::fclose(file);
            }
            vmaUnmapMemory(allocator, readbackMemory);
        }

        // --record's frame (host/record.h), shown at that aspect and drawn at
        // `at` on the game's clock.
        void writeRecord(float aspect, std::chrono::steady_clock::time_point at)
        {
            void *mapped = nullptr;
            if (vmaMapMemory(allocator, readbackMemory, &mapped) != VK_SUCCESS)
                return;
            vmaInvalidateAllocation(allocator, readbackMemory, 0, VK_WHOLE_SIZE);
            record::frame(static_cast<const uint8_t *>(mapped), targetWidth, targetHeight, aspect, at, gamePeriod);
            vmaUnmapMemory(allocator, readbackMemory);
        }
    };

    void requestShot(const std::string &name)
    {
        std::lock_guard<std::mutex> lock(s_shotMutex);
        s_shotRequest = name;
    }

    bool shotPending()
    {
        std::lock_guard<std::mutex> lock(s_shotMutex);
        return !s_shotRequest.empty() || s_shotWriting;
    }

    VulkanFrontend::VulkanFrontend(RenderSize renderSize) : m_renderSize(renderSize) {}
    VulkanFrontend::~VulkanFrontend() = default;

    bool VulkanFrontend::initialize(PS2Runtime &runtime, const char *title)
    {
        m_state = std::make_unique<State>();
        State &s = *m_state;
        s.frameLog.open();
        const PS2Runtime::HostOptions &options = runtime.hostOptions();
        s.hidden = options.hidden;
        s.shotDir = options.shotDir;
        setDrawDumpDir(options.shotDir);
        s.shotEvery = options.shotEvery;

        // MoltenVK is in the app bundle, where SDL's own search does not look.
        // A path also keeps SDL from taking volk's vkGetInstanceProcAddr
        // pointer, which the executable exports, for the loader's function.
#ifdef __APPLE__
        const char *bundle = SDL_GetBasePath();
        const std::string moltenVk = std::string(bundle ? bundle : "") + "../Frameworks/libMoltenVK.dylib";
        SDL_SetHint(SDL_HINT_VULKAN_LIBRARY, moltenVk.c_str());
        // Its errors and warnings, not the 150 lines of extensions and GPU
        // features it lists as information at every start. One set in the
        // environment stays.
        setenv("MVK_CONFIG_LOG_LEVEL", "2", 0);
#endif
        if (!SDL_Init(SDL_INIT_VIDEO))
        {
            std::cerr << "[sdl] init: " << SDL_GetError() << std::endl;
            return false;
        }
        SDL_WindowFlags flags = SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY;
        if (s.hidden)
            flags |= SDL_WINDOW_HIDDEN;
        const int width = settings::get(settings::kWidescreen) ? kWindowWidthWide : kWindowWidth;
        s.window = SDL_CreateWindow(title, width, kWindowHeight, flags);
        if (!s.window)
        {
            std::cerr << "[sdl] window: " << SDL_GetError() << std::endl;
            return false;
        }
        if (timing::fullscreen)
        {
            SDL_SetWindowFullscreen(s.window, true);
            SDL_SyncWindow(s.window);
        }
        // Also the Dock icon on macOS, which shows it as given, so it gets the
        // version already shaped like a macOS icon (tools/make_icons.sh). A
        // missing one leaves SDL's default.
#ifdef __APPLE__
        constexpr const char *kIcon = "icon_macos.png";
#else
        constexpr const char *kIcon = "icon.png";
#endif
        const char *base = SDL_GetBasePath();
        const std::string iconPath = std::string(base ? base : "") + kIcon;
        if (SDL_Surface *icon = SDL_LoadPNG(iconPath.c_str()))
        {
            SDL_SetWindowIcon(s.window, icon);
            SDL_DestroySurface(icon);
        }
        else
        {
            std::cerr << "[sdl] icon: " << SDL_GetError() << std::endl;
        }

        // The window loaded the Vulkan library; volk takes its entry point.
        volkInitializeCustom(reinterpret_cast<PFN_vkGetInstanceProcAddr>(SDL_Vulkan_GetVkGetInstanceProcAddr()));
        Uint32 extensionCount = 0;
        const char *const *extensions = SDL_Vulkan_GetInstanceExtensions(&extensionCount);
        auto instance = vkb::InstanceBuilder(vkGetInstanceProcAddr)
                            .set_app_name(title)
                            .require_api_version(1, 0, 0)
                            .enable_extensions(extensionCount, extensions)
                            .build();
        if (!instance)
        {
            std::cerr << "[vulkan] instance: " << instance.error().message() << std::endl;
            return false;
        }
        s.instance = instance.value();
        volkLoadInstance(s.instance.instance);
        if (!SDL_Vulkan_CreateSurface(s.window, s.instance.instance, nullptr, &s.surface))
        {
            std::cerr << "[sdl] surface: " << SDL_GetError() << std::endl;
            return false;
        }

        auto physical = vkb::PhysicalDeviceSelector(s.instance, s.surface).set_minimum_version(1, 0).select();
        if (!physical)
        {
            std::cerr << "[vulkan] device: " << physical.error().message() << std::endl;
            return false;
        }
        // Shading at every sample of a pixel, where the GPU has it: the 360's
        // highlights need it (SceneRenderer::initialize).
        VkPhysicalDeviceFeatures wanted{};
        wanted.sampleRateShading = VK_TRUE;
        const bool everySample = physical.value().enable_features_if_present(wanted);
        // The near plane's clip in mesh.vert and x360.vert.
        VkPhysicalDeviceFeatures clip{};
        clip.shaderClipDistance = VK_TRUE;
        physical.value().enable_features_if_present(clip);
        const bool displayTiming = timing::on() && physical.value().enable_extension_if_present("VK_GOOGLE_display_timing");
        auto device = vkb::DeviceBuilder(physical.value()).build();
        if (!device)
        {
            std::cerr << "[vulkan] device: " << device.error().message() << std::endl;
            return false;
        }
        s.device = device.value();
#if defined(__APPLE__) || defined(__linux__)
        if (displayTiming)
        {
            s.pastTiming = reinterpret_cast<PFN_vkGetPastPresentationTimingGOOGLE>(
                vkGetDeviceProcAddr(s.device.device, "vkGetPastPresentationTimingGOOGLE"));
#ifdef __APPLE__
            s.driverClockAhead = clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - timing::now();
#else
            timespec ts{};
            clock_gettime(CLOCK_MONOTONIC, &ts);
            s.driverClockAhead = static_cast<uint64_t>(ts.tv_sec) * 1000000000ull + static_cast<uint64_t>(ts.tv_nsec) - timing::now();
#endif
        }
#else
        (void)displayTiming;
#endif
        volkLoadDevice(s.device.device);
        s.queue = s.device.get_queue(vkb::QueueType::graphics).value();
        s.queueFamily = s.device.get_queue_index(vkb::QueueType::graphics).value();

        VmaVulkanFunctions functions{};
        functions.vkGetInstanceProcAddr = s.instance.fp_vkGetInstanceProcAddr;
        functions.vkGetDeviceProcAddr = s.device.fp_vkGetDeviceProcAddr;
        VmaAllocatorCreateInfo allocatorInfo{};
        allocatorInfo.vulkanApiVersion = VK_API_VERSION_1_0;
        allocatorInfo.physicalDevice = s.device.physical_device.physical_device;
        allocatorInfo.device = s.device.device;
        allocatorInfo.instance = s.instance.instance;
        allocatorInfo.pVulkanFunctions = &functions;
        if (!check(vmaCreateAllocator(&allocatorInfo, &s.allocator), "allocator"))
            return false;

        VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        poolInfo.queueFamilyIndex = s.queueFamily;
        if (!check(vkCreateCommandPool(s.device.device, &poolInfo, nullptr, &s.commandPool), "command pool"))
            return false;
        for (State::Slot &slot : s.slots)
        {
            VkCommandBufferAllocateInfo cmdInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
            cmdInfo.commandPool = s.commandPool;
            cmdInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            cmdInfo.commandBufferCount = 1;
            VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
            fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
            VkSemaphoreCreateInfo semInfo{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
            if (!check(vkAllocateCommandBuffers(s.device.device, &cmdInfo, &slot.cmd), "command buffer") ||
                !check(vkCreateFence(s.device.device, &fenceInfo, nullptr, &slot.done), "fence") ||
                !check(vkCreateSemaphore(s.device.device, &semInfo, nullptr, &slot.imageAvailable), "semaphore"))
                return false;
        }

        // The msaa setting's samples, or the most below it the GPU draws
        // colour and depth at both.
        VkPhysicalDeviceProperties properties{};
        vkGetPhysicalDeviceProperties(s.device.physical_device.physical_device, &properties);
        const VkSampleCountFlags supported =
            properties.limits.framebufferColorSampleCounts & properties.limits.framebufferDepthSampleCounts;
        uint32_t most = 1;
        while (most < 16u && (supported & (most * 2u)))
            most *= 2u;
        setMostMsaa(static_cast<int>(most));
        const uint32_t samples = static_cast<uint32_t>(msaaSamples());
        std::cerr << "[video] msaa " << samples << "x" << std::endl;
        if (!s.scene.initialize(s.device.device, s.allocator, static_cast<VkSampleCountFlagBits>(samples), everySample))
            return false;
        if (!s.hidden && !s.createSwapchain())
            return false;
        // A hidden run is scripted; the player's controller stays out of it.
        if (!s.hidden)
            openInput();
        openAudio(options.mute);
        s.nextHiddenFrame = ps2x::host_clock::now();
        s.applyFrameRate(runtime);
        return true;
    }

    bool VulkanFrontend::frame(PS2Runtime &runtime)
    {
        State &s = *m_state;
        const bool timed = timing::on();
        static uint64_t lastLoop = 0;
        uint64_t t0 = 0;
        if (timed)
        {
            t0 = timing::now();
            if (lastLoop != 0u)
                timing::span(timing::kHostLoop, lastLoop, t0);
            lastLoop = t0;
            timing::report();
        }
        SDL_Event event;
        bool devicesChanged = false;
        while (SDL_PollEvent(&event))
        {
            if (timed && ((event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat) ||
                          event.type == SDL_EVENT_JOYSTICK_BUTTON_DOWN || event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN ||
                          event.type == SDL_EVENT_JOYSTICK_HAT_MOTION || event.type == SDL_EVENT_JOYSTICK_AXIS_MOTION ||
                          event.type == SDL_EVENT_GAMEPAD_AXIS_MOTION))
                notePressEvent(event.common.timestamp);
            if (event.type == SDL_EVENT_QUIT)
                return false;
            if (event.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED)
                s.swapchainStale = true;
            if (event.type == SDL_EVENT_WINDOW_DISPLAY_CHANGED)
                s.applyFrameRate(runtime);
            if (event.type == SDL_EVENT_JOYSTICK_ADDED || event.type == SDL_EVENT_JOYSTICK_REMOVED)
                devicesChanged = true;
            // Fullscreen: Cmd+F on macOS, F11 elsewhere. Not Alt+Enter, since
            // Enter is Start on the keyboard.
            if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat && s.window)
            {
#ifdef __APPLE__
                const bool toggle = event.key.key == SDLK_F && (event.key.mod & SDL_KMOD_GUI);
#else
                const bool toggle = event.key.key == SDLK_F11;
#endif
                if (toggle)
                    SDL_SetWindowFullscreen(s.window, !(SDL_GetWindowFlags(s.window) & SDL_WINDOW_FULLSCREEN));
                cheats::key(event.key.key);
            }
        }
        // The free camera looks about with the mouse, which stays in the
        // window, unseen, for as long as it flies.
        if (s.window && flying() != s.mouseKept)
        {
            s.mouseKept = flying();
            SDL_SetWindowRelativeMouseMode(s.window, s.mouseKept);
        }
        pollInput(devicesChanged);

        // A playing movie's picture stands in for the game's frames, which
        // stop while PlayMovie blocks the game thread. Shown at 4:3, a TV's
        // picture for the 640x448 stream.
        const bool moviePlaying = movie::latestPicture(s.moviePicture);
        // The fence wait and the acquire together: how long the GPU or the
        // presentation queue held this frame back.
        using LogClock = State::FrameLog::Clock;
        const LogClock::time_point acquireStart = LogClock::now();
        State::Slot &slot = s.slots[s.slot];
        const uint64_t tFence = timed ? timing::now() : 0u;
        vkWaitForFences(s.device.device, 1, &slot.done, VK_TRUE, UINT64_MAX);
        const uint64_t t3 = timed ? timing::now() : 0u;

        uint32_t imageIndex = 0;
        const bool present = !s.hidden;
        if (present)
        {
            if (s.swapchainStale)
            {
                vkDeviceWaitIdle(s.device.device);
                if (!s.createSwapchain())
                    return false;
            }
            const VkResult acquired = vkAcquireNextImageKHR(s.device.device, s.swapchain.swapchain, UINT64_MAX,
                                                            slot.imageAvailable, VK_NULL_HANDLE, &imageIndex);
            if (acquired == VK_ERROR_OUT_OF_DATE_KHR)
            {
                s.swapchainStale = true;
                return true;
            }
            if (acquired != VK_SUCCESS && acquired != VK_SUBOPTIMAL_KHR)
                return check(acquired, "acquire");
        }
        const uint64_t t4 = timed ? timing::now() : 0u;
        const LogClock::time_point acquireEnd = LogClock::now();

        // The game's frame, taken only now: the waits above are for the
        // display, and a frame held through them is that much older when it
        // is shown. A window takes the newest, waiting up to two game
        // frames for one, so a present follows each publish and the game
        // sets the pace; taking the newest without waiting showed a third
        // of the frames twice and dropped as many. A hidden run takes each
        // in order. With none, the last is shown again.
        bool isNew = false;
        if (!moviePlaying)
            isNew = present ? frames().latest(s.shown, std::chrono::duration_cast<std::chrono::microseconds>(
                                                           s.gamePeriod * 2))
                            : frames().next(s.shown, s.frameWait);
        const Frame &frame = s.shown;
        const float displayAspect = moviePlaying ? 4.0f / 3.0f : frame.displayAspect;
        const uint64_t tTaken = timed ? timing::now() : 0u;
        const LogClock::time_point waitEnd = LogClock::now();
        if (timed)
        {
            timing::span(timing::kWaitFence, tFence, t3);
            if (present)
                timing::span(timing::kWaitAcquire, t3, t4);
            timing::span(timing::kWaitFrame, t4, tTaken);
            timing::sample(timing::kShownAgain, isNew ? 0.0 : 1.0);
            if (isNew)
                timing::sample(timing::kQueueBehind, static_cast<double>(frames().waiting()));
        }

        // Every draw goes through the game's cameras and nothing is tied to
        // the guest's pixels, so the scene can be drawn at any size. A hidden
        // run drawing at the window's keeps the guest's, so its shots compare
        // with the reference's.
        uint32_t width = frame.width;
        uint32_t height = frame.height;
        if (moviePlaying)
        {
            width = s.moviePicture.width;
            height = s.moviePicture.height;
        }
        else if (m_renderSize.mode == RenderSize::kHeight)
        {
            height = m_renderSize.height;
            width = static_cast<uint32_t>(std::lround(height * frame.displayAspect / 2.0f)) * 2u;
        }
        else if (m_renderSize.mode == RenderSize::kWindow && present)
        {
            // The render_scale setting draws it larger or smaller; the blit
            // to the window brings it to size.
            const VkRect2D rect = s.pictureRect(frame.displayAspect);
            const uint32_t percent = static_cast<uint32_t>(settings::get(settings::kRenderScale));
            width = std::max(rect.extent.width * percent / 100u, 1u);
            height = std::max(rect.extent.height * percent / 100u, 1u);
        }
        if (!s.ensureTarget(width, height))
            return false;
        vkResetFences(s.device.device, 1, &slot.done);

        ++s.presented;
        std::string requested;
        {
            std::lock_guard<std::mutex> lock(s_shotMutex);
            requested.swap(s_shotRequest);
            s_shotWriting = !requested.empty();
        }
        const bool shot = !s.shotDir.empty() &&
                          (!requested.empty() || (s.shotEvery != 0u && s.presented % s.shotEvery == 0u));

        VkCommandBuffer cmd = slot.cmd;
        vkResetCommandBuffer(cmd, 0);
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(cmd, &begin);

        const LogClock::time_point recordStart = LogClock::now();
        if (moviePlaying ? !s.recordMovie(cmd, slot)
                         : !s.scene.record(cmd, frame, s.target, s.targetWidth, s.targetHeight, s.presented))
            return false;
        const LogClock::time_point recordEnd = LogClock::now();
        if (!moviePlaying && !present &&
            !s.recordOverlay(cmd, slot, s.target, {{0, 0}, {s.targetWidth, s.targetHeight}}, false))
            return false;

        const bool recording = record::wanted();
        // A hidden frame's own slot on the game's clock, which a late wake
        // does not move; a shown one's, when it is drawn.
        const auto drawnAt = present ? ps2x::host_clock::now() : s.nextHiddenFrame;
        if (shot || recording)
        {
            VkBufferImageCopy copy{};
            copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            copy.imageExtent = {s.targetWidth, s.targetHeight, 1};
            vkCmdCopyImageToBuffer(cmd, s.target, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, s.readback, 1, &copy);
        }
        if (present && !s.blitToSwapchain(cmd, slot, s.swapchainImages[imageIndex], displayAspect, !moviePlaying))
            return false;
        vkEndCommandBuffer(cmd);
        const uint64_t tRecorded = timed ? timing::now() : 0u;

        const VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &cmd;
        if (present)
        {
            submit.waitSemaphoreCount = 1;
            submit.pWaitSemaphores = &slot.imageAvailable;
            submit.pWaitDstStageMask = &waitStage;
            submit.signalSemaphoreCount = 1;
            submit.pSignalSemaphores = &s.renderFinished[imageIndex];
        }
        if (!check(vkQueueSubmit(s.queue, 1, &submit, slot.done), "submit"))
            return false;
        const uint64_t t5 = timed ? timing::now() : 0u;
        if (timed)
        {
            timing::span(timing::kRecord, tTaken, tRecorded);
            timing::span(timing::kSubmit, tRecorded, t5);
        }

        if (present)
        {
            VkPresentInfoKHR info{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
            VkPresentTimeGOOGLE presentTime{++s.presentId, 0u};
            VkPresentTimesInfoGOOGLE presentTimes{VK_STRUCTURE_TYPE_PRESENT_TIMES_INFO_GOOGLE, nullptr, 1u, &presentTime};
            if (s.pastTiming)
                info.pNext = &presentTimes;
            info.waitSemaphoreCount = 1;
            info.pWaitSemaphores = &s.renderFinished[imageIndex];
            info.swapchainCount = 1;
            info.pSwapchains = &s.swapchain.swapchain;
            info.pImageIndices = &imageIndex;
            const VkResult presented = vkQueuePresentKHR(s.queue, &info);
            if (presented == VK_ERROR_OUT_OF_DATE_KHR || presented == VK_SUBOPTIMAL_KHR)
                s.swapchainStale = true;
        }
        s.times.add(isNew || moviePlaying, static_cast<float>(State::FrameLog::ms(waitEnd - acquireEnd)),
                    static_cast<float>(State::FrameLog::ms(recordEnd - recordStart)),
                    moviePlaying ? SceneStats{} : s.scene.stats());
        if (s.frameLog.on)
            s.frameLog.add(isNew, frames().published(), State::FrameLog::ms(waitEnd - acquireEnd),
                           State::FrameLog::ms(acquireEnd - acquireStart),
                           State::FrameLog::ms(recordEnd - recordStart),
                           moviePlaying ? SceneStats{} : s.scene.stats(), runtime.eeScheduler().droppedVBlanks());
        if (timed)
        {
            const uint64_t t6 = timing::now();
            if (present)
                timing::span(timing::kPresentCall, t5, t6);
            if (isNew && frame.publishedNs != 0u)
                timing::span(timing::kFrameAge, frame.publishedNs, t6);
            if (s.pastTiming && present)
            {
                // What the driver says of presents now shown, each against
                // when its present call returned and its frame was published.
                s.presents[s.presentId % State::kPresentsKept] = {s.presentId, t6, isNew ? frame.publishedNs : 0u};
                uint32_t count = 0;
                s.pastTiming(s.device.device, s.swapchain.swapchain, &count, nullptr);
                std::vector<VkPastPresentationTimingGOOGLE> past(count);
                if (count != 0u)
                    s.pastTiming(s.device.device, s.swapchain.swapchain, &count, past.data());
                for (const VkPastPresentationTimingGOOGLE &p : past)
                {
                    const State::Present &sent = s.presents[p.presentID % State::kPresentsKept];
                    if (sent.id != p.presentID || p.actualPresentTime == 0u)
                        continue;
                    const uint64_t shown = p.actualPresentTime - s.driverClockAhead;
                    timing::span(timing::kPresentToScreen, sent.calledNs, shown);
                    if (sent.publishedNs != 0u)
                        timing::span(timing::kPublishToScreen, sent.publishedNs, shown);
                }
            }
            if (isNew && frame.pressReadNs != 0u)
            {
                timing::span(timing::kReadToPresent, frame.pressReadNs, t6);
                if (frame.pressEventNs != 0u)
                    timing::span(timing::kEventToPresent, frame.pressEventNs, t6);
            }
        }

        if (shot || recording)
            vkWaitForFences(s.device.device, 1, &slot.done, VK_TRUE, UINT64_MAX);
        if (shot)
            s.writeShot(requested);
        if (recording)
            s.writeRecord(displayAspect, drawnAt);
        if (!requested.empty())
        {
            std::lock_guard<std::mutex> lock(s_shotMutex);
            s_shotWriting = false;
        }

        s.slot = (s.slot + 1u) % kFramesInFlight;

        // No swapchain paces a hidden window, so hold it to the game's rate.
        if (!present)
        {
            s.nextHiddenFrame += s.gamePeriod;
            const auto now = ps2x::host_clock::now();
            if (s.nextHiddenFrame < now)
                s.nextHiddenFrame = now;
            std::this_thread::sleep_until(ps2x::host_clock::real(s.nextHiddenFrame));
        }
        return true;
    }

    void VulkanFrontend::shutdown(PS2Runtime &)
    {
        if (!m_state)
            return;
        State &s = *m_state;
        record::finish();
        if (perf::logging())
            std::cerr << s.times.summary();
        if (s.device.device != VK_NULL_HANDLE)
        {
            vkDeviceWaitIdle(s.device.device);
            s.scene.shutdown();
            s.destroyTarget();
            s.destroySwapchain();
            for (State::Slot &slot : s.slots)
            {
                vkDestroyFence(s.device.device, slot.done, nullptr);
                if (slot.upload != VK_NULL_HANDLE)
                    vmaDestroyBuffer(s.allocator, slot.upload, slot.uploadMemory);
                vkDestroySemaphore(s.device.device, slot.imageAvailable, nullptr);
            }
            vkDestroyCommandPool(s.device.device, s.commandPool, nullptr);
            if (s.allocator != VK_NULL_HANDLE)
                vmaDestroyAllocator(s.allocator);
            vkb::destroy_device(s.device);
        }
        if (s.surface != VK_NULL_HANDLE)
            vkb::destroy_surface(s.instance, s.surface);
        if (s.instance.instance != VK_NULL_HANDLE)
            vkb::destroy_instance(s.instance);
        closeAudio();
        closeInput();
        if (s.window)
            SDL_DestroyWindow(s.window);
        SDL_Quit();
        m_state.reset();
    }
}
