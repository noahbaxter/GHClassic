#include "host/scene_renderer.h"

#include "render/camera.h"

#include <vk_mem_alloc.h>

#include "mesh_frag.h"
#include "mesh_vert.h"

#include <cstring>
#include <iostream>
#include <unordered_map>

namespace gh2
{
    namespace
    {
        constexpr VkFormat kColorFormat = VK_FORMAT_R8G8B8A8_UNORM;
        constexpr VkFormat kDepthFormat = VK_FORMAT_D32_SFLOAT;
        // Frames a GPU resource must outlive its last use by.
        constexpr uint64_t kRetireAfter = 3;

        struct PushConstants
        {
            float mvp[16];
            float color[4];
        };

        struct GpuMesh
        {
            std::shared_ptr<const MeshData> source; // keeps the key alive
            VkBuffer vertices = VK_NULL_HANDLE;
            VmaAllocation vertexMemory = VK_NULL_HANDLE;
            VkBuffer indices = VK_NULL_HANDLE;
            VmaAllocation indexMemory = VK_NULL_HANDLE;
            uint32_t indexCount = 0;
            uint64_t lastUsed = 0;
        };

        struct PackedVertex
        {
            float pos[3];
            float color[4];
            float uv[2];
        };

        bool check(VkResult result, const char *what)
        {
            if (result == VK_SUCCESS)
                return true;
            std::cerr << "[scene] " << what << " failed: " << result << std::endl;
            return false;
        }
    }

    struct SceneRenderer::State
    {
        VkDevice device = VK_NULL_HANDLE;
        VmaAllocator allocator = VK_NULL_HANDLE;
        VkRenderPass renderPass = VK_NULL_HANDLE;
        VkPipelineLayout layout = VK_NULL_HANDLE;
        VkPipeline pipeline = VK_NULL_HANDLE;

        // Attachments for the current target.
        VkImage target = VK_NULL_HANDLE;
        VkImageView targetView = VK_NULL_HANDLE;
        VkImage depth = VK_NULL_HANDLE;
        VmaAllocation depthMemory = VK_NULL_HANDLE;
        VkImageView depthView = VK_NULL_HANDLE;
        VkFramebuffer framebuffer = VK_NULL_HANDLE;
        uint32_t width = 0;
        uint32_t height = 0;

        std::unordered_map<const MeshData *, GpuMesh> meshes;

        VkShaderModule shader(const uint32_t *code, size_t bytes)
        {
            VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
            info.codeSize = bytes;
            info.pCode = code;
            VkShaderModule module = VK_NULL_HANDLE;
            check(vkCreateShaderModule(device, &info, nullptr, &module), "shader module");
            return module;
        }

        bool createRenderPass()
        {
            VkAttachmentDescription attachments[2]{};
            attachments[0].format = kColorFormat;
            attachments[0].samples = VK_SAMPLE_COUNT_1_BIT;
            attachments[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
            attachments[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            attachments[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            attachments[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
            attachments[0].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            attachments[0].finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            attachments[1].format = kDepthFormat;
            attachments[1].samples = VK_SAMPLE_COUNT_1_BIT;
            attachments[1].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
            attachments[1].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
            attachments[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            attachments[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
            attachments[1].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            attachments[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

            VkAttachmentReference colorRef{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
            VkAttachmentReference depthRef{1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
            VkSubpassDescription subpass{};
            subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
            subpass.colorAttachmentCount = 1;
            subpass.pColorAttachments = &colorRef;
            subpass.pDepthStencilAttachment = &depthRef;

            // The previous frame's blit and readback read the target.
            VkSubpassDependency before{};
            before.srcSubpass = VK_SUBPASS_EXTERNAL;
            before.dstSubpass = 0;
            before.srcStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
            before.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                                  VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
            before.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
            before.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                                   VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
            // This frame's blit and readback read it after.
            VkSubpassDependency after{};
            after.srcSubpass = 0;
            after.dstSubpass = VK_SUBPASS_EXTERNAL;
            after.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
            after.dstStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT;
            after.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            after.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            const VkSubpassDependency dependencies[] = {before, after};

            VkRenderPassCreateInfo info{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
            info.attachmentCount = 2;
            info.pAttachments = attachments;
            info.subpassCount = 1;
            info.pSubpasses = &subpass;
            info.dependencyCount = 2;
            info.pDependencies = dependencies;
            return check(vkCreateRenderPass(device, &info, nullptr, &renderPass), "render pass");
        }

        bool createPipeline()
        {
            VkPushConstantRange range{VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(PushConstants)};
            VkPipelineLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
            layoutInfo.pushConstantRangeCount = 1;
            layoutInfo.pPushConstantRanges = &range;
            if (!check(vkCreatePipelineLayout(device, &layoutInfo, nullptr, &layout), "pipeline layout"))
                return false;

            VkShaderModule vert = shader(kMeshVert, sizeof(kMeshVert));
            VkShaderModule frag = shader(kMeshFrag, sizeof(kMeshFrag));
            VkPipelineShaderStageCreateInfo stages[2]{};
            stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
            stages[0].module = vert;
            stages[0].pName = "main";
            stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
            stages[1].module = frag;
            stages[1].pName = "main";

            VkVertexInputBindingDescription binding{0, sizeof(PackedVertex), VK_VERTEX_INPUT_RATE_VERTEX};
            VkVertexInputAttributeDescription attributes[3] = {
                {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(PackedVertex, pos)},
                {1, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(PackedVertex, color)},
                {2, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(PackedVertex, uv)},
            };
            VkPipelineVertexInputStateCreateInfo vertexInput{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
            vertexInput.vertexBindingDescriptionCount = 1;
            vertexInput.pVertexBindingDescriptions = &binding;
            vertexInput.vertexAttributeDescriptionCount = 3;
            vertexInput.pVertexAttributeDescriptions = attributes;

            VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
            assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

            VkPipelineViewportStateCreateInfo viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
            viewport.viewportCount = 1;
            viewport.scissorCount = 1;

            VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
            raster.polygonMode = VK_POLYGON_MODE_FILL;
            raster.cullMode = VK_CULL_MODE_NONE;
            raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
            raster.lineWidth = 1.0f;

            VkPipelineMultisampleStateCreateInfo multisample{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
            multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

            VkPipelineDepthStencilStateCreateInfo depthState{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
            depthState.depthTestEnable = VK_FALSE;
            depthState.depthWriteEnable = VK_FALSE;
            depthState.depthCompareOp = VK_COMPARE_OP_ALWAYS;

            VkPipelineColorBlendAttachmentState blend{};
            blend.blendEnable = VK_TRUE;
            blend.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
            blend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            blend.colorBlendOp = VK_BLEND_OP_ADD;
            blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
            blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
            blend.alphaBlendOp = VK_BLEND_OP_ADD;
            blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                   VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
            VkPipelineColorBlendStateCreateInfo blendState{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
            blendState.attachmentCount = 1;
            blendState.pAttachments = &blend;

            const VkDynamicState dynamics[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
            VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
            dynamic.dynamicStateCount = 2;
            dynamic.pDynamicStates = dynamics;

            VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
            info.stageCount = 2;
            info.pStages = stages;
            info.pVertexInputState = &vertexInput;
            info.pInputAssemblyState = &assembly;
            info.pViewportState = &viewport;
            info.pRasterizationState = &raster;
            info.pMultisampleState = &multisample;
            info.pDepthStencilState = &depthState;
            info.pColorBlendState = &blendState;
            info.pDynamicState = &dynamic;
            info.layout = layout;
            info.renderPass = renderPass;
            const bool ok = check(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &info, nullptr, &pipeline),
                                  "pipeline");
            vkDestroyShaderModule(device, vert, nullptr);
            vkDestroyShaderModule(device, frag, nullptr);
            return ok;
        }

        void destroyAttachments()
        {
            if (framebuffer != VK_NULL_HANDLE)
                vkDestroyFramebuffer(device, framebuffer, nullptr);
            if (targetView != VK_NULL_HANDLE)
                vkDestroyImageView(device, targetView, nullptr);
            if (depthView != VK_NULL_HANDLE)
                vkDestroyImageView(device, depthView, nullptr);
            if (depth != VK_NULL_HANDLE)
                vmaDestroyImage(allocator, depth, depthMemory);
            framebuffer = VK_NULL_HANDLE;
            targetView = VK_NULL_HANDLE;
            depthView = VK_NULL_HANDLE;
            depth = VK_NULL_HANDLE;
            target = VK_NULL_HANDLE;
        }

        bool ensureAttachments(VkImage image, uint32_t w, uint32_t h)
        {
            if (image == target && w == width && h == height)
                return true;
            vkDeviceWaitIdle(device);
            destroyAttachments();

            VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
            view.image = image;
            view.viewType = VK_IMAGE_VIEW_TYPE_2D;
            view.format = kColorFormat;
            view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            if (!check(vkCreateImageView(device, &view, nullptr, &targetView), "target view"))
                return false;

            VkImageCreateInfo depthInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
            depthInfo.imageType = VK_IMAGE_TYPE_2D;
            depthInfo.format = kDepthFormat;
            depthInfo.extent = {w, h, 1};
            depthInfo.mipLevels = 1;
            depthInfo.arrayLayers = 1;
            depthInfo.samples = VK_SAMPLE_COUNT_1_BIT;
            depthInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
            depthInfo.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
            VmaAllocationCreateInfo alloc{};
            alloc.usage = VMA_MEMORY_USAGE_AUTO;
            if (!check(vmaCreateImage(allocator, &depthInfo, &alloc, &depth, &depthMemory, nullptr), "depth image"))
                return false;
            view.image = depth;
            view.format = kDepthFormat;
            view.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
            if (!check(vkCreateImageView(device, &view, nullptr, &depthView), "depth view"))
                return false;

            const VkImageView views[] = {targetView, depthView};
            VkFramebufferCreateInfo fb{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
            fb.renderPass = renderPass;
            fb.attachmentCount = 2;
            fb.pAttachments = views;
            fb.width = w;
            fb.height = h;
            fb.layers = 1;
            if (!check(vkCreateFramebuffer(device, &fb, nullptr, &framebuffer), "framebuffer"))
                return false;
            target = image;
            width = w;
            height = h;
            return true;
        }

        bool createBuffer(const void *data, VkDeviceSize bytes, VkBufferUsageFlags usage, VkBuffer &buffer,
                          VmaAllocation &memory)
        {
            VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
            info.size = bytes;
            info.usage = usage;
            VmaAllocationCreateInfo alloc{};
            alloc.usage = VMA_MEMORY_USAGE_AUTO;
            alloc.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
            VmaAllocationInfo allocated{};
            if (!check(vmaCreateBuffer(allocator, &info, &alloc, &buffer, &memory, &allocated), "buffer"))
                return false;
            std::memcpy(allocated.pMappedData, data, bytes);
            vmaFlushAllocation(allocator, memory, 0, VK_WHOLE_SIZE);
            return true;
        }

        GpuMesh *gpuMesh(const std::shared_ptr<const MeshData> &mesh, uint64_t serial)
        {
            auto found = meshes.find(mesh.get());
            if (found == meshes.end())
            {
                GpuMesh gpu;
                gpu.source = mesh;
                std::vector<PackedVertex> packed(mesh->verts.size());
                for (size_t i = 0; i < packed.size(); ++i)
                {
                    std::memcpy(packed[i].pos, mesh->verts[i].pos, sizeof(packed[i].pos));
                    std::memcpy(packed[i].color, mesh->verts[i].color, sizeof(packed[i].color));
                    std::memcpy(packed[i].uv, mesh->verts[i].uv, sizeof(packed[i].uv));
                }
                if (packed.empty() || mesh->indices.empty())
                    return nullptr;
                if (!createBuffer(packed.data(), packed.size() * sizeof(PackedVertex), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                                  gpu.vertices, gpu.vertexMemory) ||
                    !createBuffer(mesh->indices.data(), mesh->indices.size() * sizeof(uint16_t),
                                  VK_BUFFER_USAGE_INDEX_BUFFER_BIT, gpu.indices, gpu.indexMemory))
                    return nullptr;
                gpu.indexCount = static_cast<uint32_t>(mesh->indices.size());
                found = meshes.emplace(mesh.get(), std::move(gpu)).first;
            }
            found->second.lastUsed = serial;
            return &found->second;
        }

        void destroyMesh(GpuMesh &mesh)
        {
            vmaDestroyBuffer(allocator, mesh.vertices, mesh.vertexMemory);
            vmaDestroyBuffer(allocator, mesh.indices, mesh.indexMemory);
        }

        void retire(uint64_t serial)
        {
            for (auto it = meshes.begin(); it != meshes.end();)
            {
                if (it->second.lastUsed + kRetireAfter < serial)
                {
                    destroyMesh(it->second);
                    it = meshes.erase(it);
                }
                else
                    ++it;
            }
        }
    };

    SceneRenderer::SceneRenderer() = default;
    SceneRenderer::~SceneRenderer() = default;

    bool SceneRenderer::initialize(VkDevice device, VmaAllocator allocator)
    {
        m_state = std::make_unique<State>();
        m_state->device = device;
        m_state->allocator = allocator;
        return m_state->createRenderPass() && m_state->createPipeline();
    }

    void SceneRenderer::shutdown()
    {
        if (!m_state)
            return;
        State &s = *m_state;
        for (auto &entry : s.meshes)
            s.destroyMesh(entry.second);
        s.meshes.clear();
        s.destroyAttachments();
        if (s.pipeline != VK_NULL_HANDLE)
            vkDestroyPipeline(s.device, s.pipeline, nullptr);
        if (s.layout != VK_NULL_HANDLE)
            vkDestroyPipelineLayout(s.device, s.layout, nullptr);
        if (s.renderPass != VK_NULL_HANDLE)
            vkDestroyRenderPass(s.device, s.renderPass, nullptr);
        m_state.reset();
    }

    bool SceneRenderer::record(VkCommandBuffer cmd, const Frame &frame, VkImage target, uint32_t width,
                               uint32_t height, uint64_t serial)
    {
        State &s = *m_state;
        if (!s.ensureAttachments(target, width, height))
            return false;

        VkClearValue clears[2]{};
        for (int i = 0; i < 4; ++i)
            clears[0].color.float32[i] = frame.clear[i];
        clears[1].depthStencil = {0.0f, 0};
        VkRenderPassBeginInfo begin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        begin.renderPass = s.renderPass;
        begin.framebuffer = s.framebuffer;
        begin.renderArea = {{0, 0}, {width, height}};
        begin.clearValueCount = 2;
        begin.pClearValues = clears;
        vkCmdBeginRenderPass(cmd, &begin, VK_SUBPASS_CONTENTS_INLINE);

        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s.pipeline);

        std::vector<Matrix> viewProjections;
        viewProjections.reserve(frame.cameras.size());
        for (const Camera &camera : frame.cameras)
            viewProjections.push_back(viewProjection(camera, frame.yRatio));

        const float w = static_cast<float>(width);
        const float h = static_cast<float>(height);
        uint32_t boundCamera = UINT32_MAX;
        for (const DrawCall &draw : frame.draws)
        {
            if (!draw.mesh || draw.camera >= viewProjections.size())
                continue;
            if (frame.cameras[draw.camera].rect[2] <= 0.0f || frame.cameras[draw.camera].rect[3] <= 0.0f)
                continue;
            GpuMesh *mesh = s.gpuMesh(draw.mesh, serial);
            if (!mesh)
                continue;
            if (draw.camera != boundCamera)
            {
                // The camera's rect, as PsCam::Select sets its viewport and
                // scissor (0x19c504).
                const float *rect = frame.cameras[draw.camera].rect;
                VkViewport viewport{rect[0] * w, rect[1] * h, rect[2] * w, rect[3] * h, 0.0f, 1.0f};
                const auto clamp01 = [](float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); };
                const int32_t x0 = static_cast<int32_t>(clamp01(rect[0]) * w);
                const int32_t y0 = static_cast<int32_t>(clamp01(rect[1]) * h);
                const int32_t x1 = static_cast<int32_t>(clamp01(rect[0] + rect[2]) * w);
                const int32_t y1 = static_cast<int32_t>(clamp01(rect[1] + rect[3]) * h);
                VkRect2D scissor{{x0, y0}, {static_cast<uint32_t>(x1 > x0 ? x1 - x0 : 0),
                                            static_cast<uint32_t>(y1 > y0 ? y1 - y0 : 0)}};
                vkCmdSetViewport(cmd, 0, 1, &viewport);
                vkCmdSetScissor(cmd, 0, 1, &scissor);
                boundCamera = draw.camera;
            }
            PushConstants push{};
            const Matrix mvp = multiply(draw.world, viewProjections[draw.camera]);
            std::memcpy(push.mvp, mvp.data(), sizeof(push.mvp));
            push.color[0] = push.color[1] = push.color[2] = push.color[3] = 1.0f;
            vkCmdPushConstants(cmd, s.layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(push), &push);
            const VkDeviceSize offset = 0;
            vkCmdBindVertexBuffers(cmd, 0, 1, &mesh->vertices, &offset);
            vkCmdBindIndexBuffer(cmd, mesh->indices, 0, VK_INDEX_TYPE_UINT16);
            vkCmdDrawIndexed(cmd, mesh->indexCount, 1, 0, 0, 0);
        }

        vkCmdEndRenderPass(cmd);
        s.retire(serial);
        return true;
    }
}
