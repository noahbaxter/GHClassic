#include "host/vulkan_frontend.h"

#include "host/audio.h"
#include "host/input.h"
#include "host/scene_renderer.h"
#include "render/frame.h"
#include "runtime/ee_scheduler.h"
#include "settings.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <VkBootstrap.h>
#include <vk_mem_alloc.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace gh2
{
    namespace
    {
        constexpr uint32_t kFramesInFlight = 2;
        constexpr int kWindowWidth = 960;
        constexpr int kWindowHeight = 720;

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
        };
        std::array<Slot, kFramesInFlight> slots{};
        uint32_t slot = 0;

        VkBuffer readback = VK_NULL_HANDLE;
        VmaAllocation readbackMemory = VK_NULL_HANDLE;
        VkDeviceSize readbackSize = 0;

        std::chrono::steady_clock::time_point nextHiddenFrame{};
        Frame shown;                                 // the game frame last drawn
        std::chrono::microseconds frameWait{8333};   // half a display period
        std::chrono::nanoseconds gamePeriod{16667000};
        bool newestOnly = false;                     // the game outruns the display

        // Frame rates in the window title, every half second: new game
        // frames shown, and frames the game finished.
        std::string title;
        std::chrono::steady_clock::time_point rateSince{};
        uint64_t shownCount = 0;
        uint64_t publishedSince = 0;

        void countFrame(bool isNew)
        {
            shownCount += isNew ? 1u : 0u;
            const auto now = std::chrono::steady_clock::now();
            const double elapsed = std::chrono::duration<double>(now - rateSince).count();
            if (elapsed < 0.5)
                return;
            const uint64_t published = frames().published();
            if (window && rateSince != std::chrono::steady_clock::time_point{})
            {
                char text[160];
                std::snprintf(text, sizeof(text), "%s - %.0f fps (game %.0f)", title.c_str(),
                              shownCount / elapsed, (published - publishedSince) / elapsed);
                SDL_SetWindowTitle(window, text);
            }
            rateSince = now;
            shownCount = 0;
            publishedSince = published;
        }

        // The game's frame rate from settings, as the runtime's vblank: one
        // game frame a vblank.
        void applyFrameRate(PS2Runtime &runtime)
        {
            float displayHz = 0.0f;
            if (!hidden)
                if (const SDL_DisplayMode *mode = SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(window)))
                    displayHz = mode->refresh_rate;
            float hz = static_cast<float>(settings::get(settings::kFrameRate));
            if (hz == 0.0f)
                hz = displayHz > 0.0f ? displayHz : 60.0f;
            // frame_step replays at most four PS2 frames in one of ours.
            hz = std::max(hz, 15.0f);
            gamePeriod = std::chrono::nanoseconds(static_cast<int64_t>(1e9 / hz));
            runtime.eeScheduler().setVBlankPeriod(gamePeriod);
            if (displayHz > 0.0f)
                frameWait = std::chrono::microseconds(static_cast<int64_t>(500000.0f / displayHz));
            newestOnly = displayHz > 0.0f && hz > displayHz * 1.01f;
        }

        SceneRenderer scene;

        bool createSwapchain()
        {
            int width = 0, height = 0;
            SDL_GetWindowSizeInPixels(window, &width, &height);
            vkb::SwapchainBuilder builder(device);
            // UNORM, so the guest's display-referred colours pass through as is.
            builder.set_desired_format({VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR})
                .set_desired_present_mode(VK_PRESENT_MODE_FIFO_KHR)
                .set_desired_extent(static_cast<uint32_t>(width), static_cast<uint32_t>(height))
                .add_image_usage_flags(VK_IMAGE_USAGE_TRANSFER_DST_BIT)
                .set_old_swapchain(swapchain);
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

        // The picture into the window at pictureRect.
        void blitToSwapchain(VkCommandBuffer cmd, VkImage image, float displayAspect)
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

            imageBarrier(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                         VK_ACCESS_TRANSFER_WRITE_BIT, 0, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
        }

        // Harness screenshots, as binary PPM; scripts/arm.sh converts them.
        void writeShot()
        {
            void *mapped = nullptr;
            if (vmaMapMemory(allocator, readbackMemory, &mapped) != VK_SUCCESS)
                return;
            vmaInvalidateAllocation(allocator, readbackMemory, 0, VK_WHOLE_SIZE);
            char name[32];
            std::snprintf(name, sizeof(name), "frame_%06llu.ppm", static_cast<unsigned long long>(presented));
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
    };

    VulkanFrontend::VulkanFrontend(RenderSize renderSize) : m_renderSize(renderSize) {}
    VulkanFrontend::~VulkanFrontend() = default;

    bool VulkanFrontend::initialize(PS2Runtime &runtime, const char *title)
    {
        m_state = std::make_unique<State>();
        State &s = *m_state;
        const PS2Runtime::HostOptions &options = runtime.hostOptions();
        s.hidden = options.hidden;
        s.shotDir = options.shotDir;
        s.shotEvery = options.shotEvery;
        s.title = title;

        if (!SDL_Init(SDL_INIT_VIDEO))
        {
            std::cerr << "[sdl] init: " << SDL_GetError() << std::endl;
            return false;
        }
        SDL_WindowFlags flags = SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY;
        if (s.hidden)
            flags |= SDL_WINDOW_HIDDEN;
        s.window = SDL_CreateWindow(title, kWindowWidth, kWindowHeight, flags);
        if (!s.window)
        {
            std::cerr << "[sdl] window: " << SDL_GetError() << std::endl;
            return false;
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

        Uint32 extensionCount = 0;
        const char *const *extensions = SDL_Vulkan_GetInstanceExtensions(&extensionCount);
        auto instance = vkb::InstanceBuilder(vkGetInstanceProcAddr)
                            .set_app_name(title)
                            .require_api_version(1, 2, 0)
                            .enable_extensions(extensionCount, extensions)
                            .build();
        if (!instance)
        {
            std::cerr << "[vulkan] instance: " << instance.error().message() << std::endl;
            return false;
        }
        s.instance = instance.value();
        if (!SDL_Vulkan_CreateSurface(s.window, s.instance.instance, nullptr, &s.surface))
        {
            std::cerr << "[sdl] surface: " << SDL_GetError() << std::endl;
            return false;
        }

        auto physical = vkb::PhysicalDeviceSelector(s.instance, s.surface).set_minimum_version(1, 2).select();
        if (!physical)
        {
            std::cerr << "[vulkan] device: " << physical.error().message() << std::endl;
            return false;
        }
        auto device = vkb::DeviceBuilder(physical.value()).build();
        if (!device)
        {
            std::cerr << "[vulkan] device: " << device.error().message() << std::endl;
            return false;
        }
        s.device = device.value();
        s.queue = s.device.get_queue(vkb::QueueType::graphics).value();
        s.queueFamily = s.device.get_queue_index(vkb::QueueType::graphics).value();

        VmaVulkanFunctions functions{};
        functions.vkGetInstanceProcAddr = s.instance.fp_vkGetInstanceProcAddr;
        functions.vkGetDeviceProcAddr = s.device.fp_vkGetDeviceProcAddr;
        VmaAllocatorCreateInfo allocatorInfo{};
        allocatorInfo.vulkanApiVersion = VK_API_VERSION_1_2;
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
        uint32_t samples = 1;
        while (samples * 2u <= static_cast<uint32_t>(settings::get(settings::kMsaa)) && (supported & (samples * 2u)))
            samples *= 2u;
        std::cerr << "[video] msaa " << samples << "x" << std::endl;
        if (!s.scene.initialize(s.device.device, s.allocator, static_cast<VkSampleCountFlagBits>(samples)))
            return false;
        if (!s.hidden && !s.createSwapchain())
            return false;
        // A hidden run is scripted; the player's controller stays out of it.
        if (!s.hidden)
            openInput();
        openAudio(options.mute);
        s.nextHiddenFrame = std::chrono::steady_clock::now();
        s.applyFrameRate(runtime);
        return true;
    }

    bool VulkanFrontend::frame(PS2Runtime &runtime)
    {
        State &s = *m_state;
        SDL_Event event;
        bool devicesChanged = false;
        while (SDL_PollEvent(&event))
        {
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
            }
        }
        pollInput(devicesChanged);

        // Each game frame once, in order: wait up to half a display period for
        // the next, else show the last again. A game faster than the display
        // shows its newest.
        const bool isNew = s.newestOnly ? frames().latest(s.shown) : frames().next(s.shown, s.frameWait);
        s.countFrame(isNew);
        const Frame &frame = s.shown;

        State::Slot &slot = s.slots[s.slot];
        vkWaitForFences(s.device.device, 1, &slot.done, VK_TRUE, UINT64_MAX);

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

        // Every draw goes through the game's cameras and nothing is tied to
        // the guest's pixels, so the scene can be drawn at any size. A hidden
        // run drawing at the window's keeps the guest's, so its shots compare
        // with the reference's.
        uint32_t width = frame.width;
        uint32_t height = frame.height;
        if (m_renderSize.mode == RenderSize::kHeight)
        {
            height = m_renderSize.height;
            width = static_cast<uint32_t>(std::lround(height * frame.displayAspect / 2.0f)) * 2u;
        }
        else if (m_renderSize.mode == RenderSize::kWindow && present)
        {
            const VkRect2D rect = s.pictureRect(frame.displayAspect);
            width = std::max(rect.extent.width, 1u);
            height = std::max(rect.extent.height, 1u);
        }
        if (!s.ensureTarget(width, height))
            return false;
        vkResetFences(s.device.device, 1, &slot.done);

        ++s.presented;
        const bool shot = s.shotEvery != 0u && !s.shotDir.empty() && s.presented % s.shotEvery == 0u;

        VkCommandBuffer cmd = slot.cmd;
        vkResetCommandBuffer(cmd, 0);
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(cmd, &begin);

        if (!s.scene.record(cmd, frame, s.target, s.targetWidth, s.targetHeight, s.presented))
            return false;

        if (shot)
        {
            VkBufferImageCopy copy{};
            copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            copy.imageExtent = {s.targetWidth, s.targetHeight, 1};
            vkCmdCopyImageToBuffer(cmd, s.target, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, s.readback, 1, &copy);
        }
        if (present)
            s.blitToSwapchain(cmd, s.swapchainImages[imageIndex], frame.displayAspect);
        vkEndCommandBuffer(cmd);

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

        if (present)
        {
            VkPresentInfoKHR info{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
            info.waitSemaphoreCount = 1;
            info.pWaitSemaphores = &s.renderFinished[imageIndex];
            info.swapchainCount = 1;
            info.pSwapchains = &s.swapchain.swapchain;
            info.pImageIndices = &imageIndex;
            const VkResult presented = vkQueuePresentKHR(s.queue, &info);
            if (presented == VK_ERROR_OUT_OF_DATE_KHR || presented == VK_SUBOPTIMAL_KHR)
                s.swapchainStale = true;
        }

        if (shot)
        {
            vkWaitForFences(s.device.device, 1, &slot.done, VK_TRUE, UINT64_MAX);
            s.writeShot();
        }

        s.slot = (s.slot + 1u) % kFramesInFlight;

        // No swapchain paces a hidden window, so hold it to the game's rate.
        if (!present)
        {
            s.nextHiddenFrame += s.gamePeriod;
            const auto now = std::chrono::steady_clock::now();
            if (s.nextHiddenFrame < now)
                s.nextHiddenFrame = now;
            std::this_thread::sleep_until(s.nextHiddenFrame);
        }
        return true;
    }

    void VulkanFrontend::shutdown(PS2Runtime &)
    {
        if (!m_state)
            return;
        State &s = *m_state;
        if (s.device.device != VK_NULL_HANDLE)
        {
            vkDeviceWaitIdle(s.device.device);
            s.scene.shutdown();
            s.destroyTarget();
            s.destroySwapchain();
            for (State::Slot &slot : s.slots)
            {
                vkDestroyFence(s.device.device, slot.done, nullptr);
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
