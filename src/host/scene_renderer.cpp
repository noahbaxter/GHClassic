#include "host/scene_renderer.h"

#include "milo/layout.h"
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

        // How a vertex gets its colour, by the VU1 lighting program the
        // material and environ pick (PsMat::Select 0x3d8548).
        enum ColorMode : uint32_t
        {
            kColorVertex,      // 0x614 (prelit, no environ): as is
            kColorAmbient,     // 0x7c5 with the environ: base * ambient
            kColorDirectional, // 0x6ec: base * ambient + lights * material
            kColorMaterial,    // 0x7c5 without the environ or prelit: the material's
            kColorPoint,       // 0x436: base * ambient + a point light * material, within its range
        };
        constexpr uint32_t kFlagPrelit = 1u << 3;    // base is the vertex colour, else the material's
        constexpr uint32_t kFlagAlphaCut = 1u << 4;  // discard alpha below the GS's 1
        constexpr uint32_t kFlagIntensify = 1u << 5; // textured rgb scale 255 over 128

        struct PushConstants
        {
            float mvp[16];
            float matColor[4];
            float uvRows[4]; // Material::uvXfm
            float uvOffset[2];
            int32_t boneBase;  // the draw's first bone vec4 in the frame data, -1 when rigid
            int32_t lightBase; // the draw's lighting block in the frame data
            uint32_t flags;    // ColorMode in bits 0-2, then the kFlag bits
        };
        static_assert(sizeof(PushConstants) <= 128, "past Vulkan's guaranteed push constant size");

        // Each frame writes its bones and lighting to its own slot's buffer,
        // so a slot is reused only after its frame has retired.
        constexpr uint32_t kFrameSlots = kRetireAfter + 1u;
        constexpr uint32_t kMinFrameData = 1024u; // vec4s
        // A draw's lighting block: ambient, three light colours, then the
        // three directions to the lights in the mesh's own space.
        constexpr uint32_t kLightingVec4s = 7u;

        constexpr uint32_t kPipelineCount = milo::mat::kBlendCount * milo::mat::kZModeCount;

        // The GS's ALPHA_1 for each blend, as PsMat::Update sets it.
        VkPipelineColorBlendAttachmentState blendState(uint32_t blend)
        {
            VkPipelineColorBlendAttachmentState state{};
            state.blendEnable = VK_TRUE;
            state.colorBlendOp = VK_BLEND_OP_ADD;
            state.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
            state.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
            state.alphaBlendOp = VK_BLEND_OP_ADD;
            state.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                   VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
            switch (blend)
            {
            case milo::mat::kBlendDest:
                state.srcColorBlendFactor = VK_BLEND_FACTOR_ZERO;
                state.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
                state.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
                state.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
                break;
            case milo::mat::kBlendSrc:
                state.blendEnable = VK_FALSE;
                break;
            case milo::mat::kBlendAdd:
                state.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
                state.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
                break;
            case milo::mat::kBlendSrcAlphaAdd:
                state.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
                state.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
                break;
            case milo::mat::kBlendSubtract:
                state.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
                state.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
                state.colorBlendOp = VK_BLEND_OP_REVERSE_SUBTRACT;
                break;
            default: // kBlendSrcAlpha
                state.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
                state.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
                break;
            }
            return state;
        }

        // The GS's ZTST and ZMSK for each z mode. Depth is reversed like the
        // GS's, nearer is larger.
        VkPipelineDepthStencilStateCreateInfo depthState(uint32_t zMode)
        {
            VkPipelineDepthStencilStateCreateInfo state{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
            state.depthTestEnable = VK_TRUE;
            switch (zMode)
            {
            case milo::mat::kZDisable:
                state.depthCompareOp = VK_COMPARE_OP_ALWAYS;
                break;
            case milo::mat::kZTransparent:
                state.depthCompareOp = VK_COMPARE_OP_GREATER_OR_EQUAL;
                break;
            case milo::mat::kZForce:
                state.depthCompareOp = VK_COMPARE_OP_ALWAYS;
                state.depthWriteEnable = VK_TRUE;
                break;
            case milo::mat::kZDecal:
                state.depthCompareOp = VK_COMPARE_OP_GREATER_OR_EQUAL;
                state.depthWriteEnable = VK_TRUE;
                break;
            default: // kZNormal
                state.depthCompareOp = VK_COMPARE_OP_GREATER;
                state.depthWriteEnable = VK_TRUE;
                break;
            }
            return state;
        }

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
            float normal[3];
            float color[4];
            float uv[2];
        };

        struct GpuTexture
        {
            std::shared_ptr<const TextureData> source; // keeps the key alive
            VkImage image = VK_NULL_HANDLE;
            VmaAllocation memory = VK_NULL_HANDLE;
            VkImageView view = VK_NULL_HANDLE;
            VkDescriptorSet sets[2]{}; // clamped, wrapped
            VkBuffer staging = VK_NULL_HANDLE;
            VmaAllocation stagingMemory = VK_NULL_HANDLE;
            uint64_t uploaded = 0;
            uint64_t lastUsed = 0;
        };

        // Room for this many live textures before the pool refuses.
        constexpr uint32_t kMaxTextures = 4096;

        struct Vec4
        {
            float v[4];
        };

        struct FrameDataSlot
        {
            VkBuffer buffer = VK_NULL_HANDLE;
            VmaAllocation memory = VK_NULL_HANDLE;
            Vec4 *mapped = nullptr;
            uint32_t capacity = 0; // vec4s
            VkDescriptorSet set = VK_NULL_HANDLE;
        };

        // The lighting program a draw runs. PsMat::Update counts a material
        // lit when it has use_environ or is not prelit (0x19d12c). Select
        // passes the environ's program for use_environ, else 0x7c5 with
        // use_environ clear, which stores the material colour, clamped to 1,
        // to every vert (0x3e58). An unlit material, prelit alone, runs 0x614:
        // the vertex colour as it is.
        uint32_t colorMode(const DrawCall &draw)
        {
            if (!draw.material.useEnviron)
                return draw.material.prelit ? kColorVertex : kColorMaterial;
            if (draw.environ.kind == Environ::kPoint)
                return kColorPoint;
            return draw.environ.kind == Environ::kDirectional ? kColorDirectional : kColorAmbient;
        }

        // Ambient (w 1), the three light colours (w 0, unused slots black),
        // then each light's direction taken into the mesh's space through the
        // rows of its lighting matrix, as the directional program does before
        // its loop (0x3788..0x37c8): d = n . direction.
        //
        // A point light fills the same block as the point program reads it
        // (0x21b0..0x2238): its colour in [1], its position taken relative to
        // the mesh's origin (qw679) and into the mesh's space through the rows
        // in [4] with -1/range in w, and range squared in [5] w.
        void writeLighting(const DrawCall &draw, Vec4 *out)
        {
            const Environ &e = draw.environ;
            out[0] = {{e.ambient[0], e.ambient[1], e.ambient[2], 1.0f}};
            if (e.kind == Environ::kPoint)
            {
                out[1] = {{e.color[0][0], e.color[0][1], e.color[0][2], 0.0f}};
                out[2] = out[3] = out[6] = {{0.0f, 0.0f, 0.0f, 0.0f}};
                float rel[3];
                for (uint32_t c = 0; c < 3; ++c)
                    rel[c] = e.position[c] - draw.lightWorld[12 + c];
                Vec4 local{{0.0f, 0.0f, 0.0f, e.range != 0.0f ? -1.0f / e.range : 0.0f}};
                for (uint32_t row = 0; row < 3; ++row)
                    local.v[row] = draw.lightWorld[row * 4 + 0] * rel[0] + draw.lightWorld[row * 4 + 1] * rel[1] +
                                   draw.lightWorld[row * 4 + 2] * rel[2];
                out[4] = local;
                out[5] = {{0.0f, 0.0f, 0.0f, e.range * e.range}};
                return;
            }
            for (uint32_t i = 0; i < 3; ++i)
            {
                const bool present = i < e.lightCount;
                out[1 + i] = {{present ? e.color[i][0] : 0.0f, present ? e.color[i][1] : 0.0f,
                               present ? e.color[i][2] : 0.0f, 0.0f}};
                Vec4 direction{{0.0f, 0.0f, 0.0f, 0.0f}};
                if (present)
                    for (uint32_t row = 0; row < 3; ++row)
                        direction.v[row] = draw.lightWorld[row * 4 + 0] * e.toLight[i][0] +
                                           draw.lightWorld[row * 4 + 1] * e.toLight[i][1] +
                                           draw.lightWorld[row * 4 + 2] * e.toLight[i][2];
                out[4 + i] = direction;
            }
        }

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
        VkShaderModule vertexShader = VK_NULL_HANDLE;
        VkShaderModule fragmentShader = VK_NULL_HANDLE;
        // By blend and z mode, built on first use.
        VkPipeline pipelines[kPipelineCount]{};

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

        VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
        VkDescriptorPool pool = VK_NULL_HANDLE;
        VkSampler samplers[2]{}; // clamped, wrapped
        std::unordered_map<const TextureData *, GpuTexture> textures;
        // What an untextured pass samples, so every pass multiplies by a
        // texture.
        std::shared_ptr<const TextureData> white;

        VkDescriptorSetLayout frameDataSetLayout = VK_NULL_HANDLE;
        FrameDataSlot frameData[kFrameSlots];

        bool createDescriptorState()
        {
            VkDescriptorSetLayoutBinding binding{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
                                                 VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
            VkDescriptorSetLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
            layoutInfo.bindingCount = 1;
            layoutInfo.pBindings = &binding;
            if (!check(vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &setLayout), "set layout"))
                return false;
            VkDescriptorSetLayoutBinding frameDataBinding{0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
                                                          VK_SHADER_STAGE_VERTEX_BIT, nullptr};
            layoutInfo.pBindings = &frameDataBinding;
            if (!check(vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &frameDataSetLayout),
                       "frame data set layout"))
                return false;

            const VkDescriptorPoolSize sizes[] = {
                {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kMaxTextures * 2u},
                {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, kFrameSlots},
            };
            VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
            poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
            poolInfo.maxSets = kMaxTextures * 2u + kFrameSlots;
            poolInfo.poolSizeCount = 2;
            poolInfo.pPoolSizes = sizes;
            if (!check(vkCreateDescriptorPool(device, &poolInfo, nullptr, &pool), "descriptor pool"))
                return false;

            for (FrameDataSlot &slot : frameData)
            {
                VkDescriptorSetAllocateInfo setInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
                setInfo.descriptorPool = pool;
                setInfo.descriptorSetCount = 1;
                setInfo.pSetLayouts = &frameDataSetLayout;
                if (!check(vkAllocateDescriptorSets(device, &setInfo, &slot.set), "frame data set") ||
                    !growFrameData(slot, kMinFrameData))
                    return false;
            }

            // PsTex sets TEX1 to bilinear with no mips (SyncBitmap 0x1a14b8).
            for (int wrap = 0; wrap < 2; ++wrap)
            {
                const VkSamplerAddressMode mode =
                    wrap ? VK_SAMPLER_ADDRESS_MODE_REPEAT : VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
                VkSamplerCreateInfo info{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
                info.magFilter = VK_FILTER_LINEAR;
                info.minFilter = VK_FILTER_LINEAR;
                info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
                info.addressModeU = mode;
                info.addressModeV = mode;
                info.addressModeW = mode;
                info.maxLod = 0.0f;
                if (!check(vkCreateSampler(device, &info, nullptr, &samplers[wrap]), "sampler"))
                    return false;
            }

            auto pixel = std::make_shared<TextureData>();
            pixel->width = 1;
            pixel->height = 1;
            pixel->rgba = {255, 255, 255, 255};
            white = std::move(pixel);
            return true;
        }

        // Replaces the slot's buffer with one holding at least `count`
        // vec4s. Only called on a slot no frame in flight uses.
        bool growFrameData(FrameDataSlot &slot, uint32_t count)
        {
            if (count <= slot.capacity)
                return true;
            uint32_t capacity = slot.capacity ? slot.capacity : kMinFrameData;
            while (capacity < count)
                capacity *= 2u;
            if (slot.buffer != VK_NULL_HANDLE)
                vmaDestroyBuffer(allocator, slot.buffer, slot.memory);
            slot = FrameDataSlot{VK_NULL_HANDLE, VK_NULL_HANDLE, nullptr, 0, slot.set};

            VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
            info.size = static_cast<VkDeviceSize>(capacity) * sizeof(Vec4);
            info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
            VmaAllocationCreateInfo alloc{};
            alloc.usage = VMA_MEMORY_USAGE_AUTO;
            alloc.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
            VmaAllocationInfo allocated{};
            if (!check(vmaCreateBuffer(allocator, &info, &alloc, &slot.buffer, &slot.memory, &allocated), "frame data"))
                return false;
            slot.mapped = static_cast<Vec4 *>(allocated.pMappedData);
            slot.capacity = capacity;

            VkDescriptorBufferInfo buffer{slot.buffer, 0, VK_WHOLE_SIZE};
            VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            write.dstSet = slot.set;
            write.descriptorCount = 1;
            write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            write.pBufferInfo = &buffer;
            vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
            return true;
        }

        void destroyTexture(GpuTexture &texture)
        {
            vkFreeDescriptorSets(device, pool, 2, texture.sets);
            vkDestroyImageView(device, texture.view, nullptr);
            vmaDestroyImage(allocator, texture.image, texture.memory);
            if (texture.staging != VK_NULL_HANDLE)
                vmaDestroyBuffer(allocator, texture.staging, texture.stagingMemory);
        }

        // The texture's GPU copy, uploading it through `cmd` the first time.
        // Called outside the render pass.
        GpuTexture *gpuTexture(VkCommandBuffer cmd, const std::shared_ptr<const TextureData> &data, uint64_t serial)
        {
            auto found = textures.find(data.get());
            if (found != textures.end())
            {
                found->second.lastUsed = serial;
                return &found->second;
            }

            GpuTexture gpu;
            gpu.source = data;
            gpu.uploaded = serial;
            gpu.lastUsed = serial;
            const VkDeviceSize bytes = data->rgba.size();
            if (!createBuffer(data->rgba.data(), bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, gpu.staging,
                              gpu.stagingMemory))
                return nullptr;

            VkImageCreateInfo imageInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
            imageInfo.imageType = VK_IMAGE_TYPE_2D;
            imageInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
            imageInfo.extent = {data->width, data->height, 1};
            imageInfo.mipLevels = 1;
            imageInfo.arrayLayers = 1;
            imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
            imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
            imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
            VmaAllocationCreateInfo alloc{};
            alloc.usage = VMA_MEMORY_USAGE_AUTO;
            if (!check(vmaCreateImage(allocator, &imageInfo, &alloc, &gpu.image, &gpu.memory, nullptr), "texture"))
            {
                vmaDestroyBuffer(allocator, gpu.staging, gpu.stagingMemory);
                return nullptr;
            }

            VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
            view.image = gpu.image;
            view.viewType = VK_IMAGE_VIEW_TYPE_2D;
            view.format = VK_FORMAT_R8G8B8A8_UNORM;
            view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            check(vkCreateImageView(device, &view, nullptr, &gpu.view), "texture view");

            const VkDescriptorSetLayout layouts[2] = {setLayout, setLayout};
            VkDescriptorSetAllocateInfo setInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
            setInfo.descriptorPool = pool;
            setInfo.descriptorSetCount = 2;
            setInfo.pSetLayouts = layouts;
            if (!check(vkAllocateDescriptorSets(device, &setInfo, gpu.sets), "descriptor sets"))
            {
                vkDestroyImageView(device, gpu.view, nullptr);
                vmaDestroyImage(allocator, gpu.image, gpu.memory);
                vmaDestroyBuffer(allocator, gpu.staging, gpu.stagingMemory);
                return nullptr;
            }
            for (int wrap = 0; wrap < 2; ++wrap)
            {
                VkDescriptorImageInfo image{samplers[wrap], gpu.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
                VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
                write.dstSet = gpu.sets[wrap];
                write.descriptorCount = 1;
                write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                write.pImageInfo = &image;
                vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
            }

            VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
            barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.image = gpu.image;
            barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0,
                                 nullptr, 0, nullptr, 1, &barrier);
            VkBufferImageCopy copy{};
            copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            copy.imageExtent = {data->width, data->height, 1};
            vkCmdCopyBufferToImage(cmd, gpu.staging, gpu.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
            barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0,
                                 nullptr, 0, nullptr, 1, &barrier);

            return &textures.emplace(data.get(), std::move(gpu)).first->second;
        }

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

        bool createLayout()
        {
            VkPushConstantRange range{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                                      sizeof(PushConstants)};
            VkPipelineLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
            layoutInfo.pushConstantRangeCount = 1;
            layoutInfo.pPushConstantRanges = &range;
            const VkDescriptorSetLayout setLayouts[] = {setLayout, frameDataSetLayout};
            layoutInfo.setLayoutCount = 2;
            layoutInfo.pSetLayouts = setLayouts;
            if (!check(vkCreatePipelineLayout(device, &layoutInfo, nullptr, &layout), "pipeline layout"))
                return false;
            vertexShader = shader(kMeshVert, sizeof(kMeshVert));
            fragmentShader = shader(kMeshFrag, sizeof(kMeshFrag));
            return vertexShader != VK_NULL_HANDLE && fragmentShader != VK_NULL_HANDLE;
        }

        VkPipeline pipeline(uint32_t blend, uint32_t zMode)
        {
            VkPipeline &slot = pipelines[blend * milo::mat::kZModeCount + zMode];
            if (slot != VK_NULL_HANDLE)
                return slot;

            VkPipelineShaderStageCreateInfo stages[2]{};
            stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
            stages[0].module = vertexShader;
            stages[0].pName = "main";
            stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
            stages[1].module = fragmentShader;
            stages[1].pName = "main";

            VkVertexInputBindingDescription binding{0, sizeof(PackedVertex), VK_VERTEX_INPUT_RATE_VERTEX};
            VkVertexInputAttributeDescription attributes[4] = {
                {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(PackedVertex, pos)},
                {1, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(PackedVertex, color)},
                {2, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(PackedVertex, uv)},
                {3, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(PackedVertex, normal)},
            };
            VkPipelineVertexInputStateCreateInfo vertexInput{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
            vertexInput.vertexBindingDescriptionCount = 1;
            vertexInput.pVertexBindingDescriptions = &binding;
            vertexInput.vertexAttributeDescriptionCount = 4;
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

            const VkPipelineDepthStencilStateCreateInfo depth = depthState(zMode);

            const VkPipelineColorBlendAttachmentState attachment = blendState(blend);
            VkPipelineColorBlendStateCreateInfo colorBlend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
            colorBlend.attachmentCount = 1;
            colorBlend.pAttachments = &attachment;

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
            info.pDepthStencilState = &depth;
            info.pColorBlendState = &colorBlend;
            info.pDynamicState = &dynamic;
            info.layout = layout;
            info.renderPass = renderPass;
            check(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &info, nullptr, &slot), "pipeline");
            return slot;
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
                    std::memcpy(packed[i].normal, mesh->verts[i].normal, sizeof(packed[i].normal));
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
            for (auto it = textures.begin(); it != textures.end();)
            {
                GpuTexture &texture = it->second;
                if (texture.lastUsed + kRetireAfter < serial)
                {
                    destroyTexture(texture);
                    it = textures.erase(it);
                    continue;
                }
                if (texture.staging != VK_NULL_HANDLE && texture.uploaded + kRetireAfter < serial)
                {
                    vmaDestroyBuffer(allocator, texture.staging, texture.stagingMemory);
                    texture.staging = VK_NULL_HANDLE;
                    texture.stagingMemory = VK_NULL_HANDLE;
                }
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
        return m_state->createRenderPass() && m_state->createDescriptorState() && m_state->createLayout();
    }

    void SceneRenderer::shutdown()
    {
        if (!m_state)
            return;
        State &s = *m_state;
        for (auto &entry : s.meshes)
            s.destroyMesh(entry.second);
        s.meshes.clear();
        for (auto &entry : s.textures)
            s.destroyTexture(entry.second);
        s.textures.clear();
        for (VkSampler sampler : s.samplers)
            if (sampler != VK_NULL_HANDLE)
                vkDestroySampler(s.device, sampler, nullptr);
        for (FrameDataSlot &slot : s.frameData)
            if (slot.buffer != VK_NULL_HANDLE)
                vmaDestroyBuffer(s.allocator, slot.buffer, slot.memory);
        if (s.pool != VK_NULL_HANDLE)
            vkDestroyDescriptorPool(s.device, s.pool, nullptr);
        if (s.setLayout != VK_NULL_HANDLE)
            vkDestroyDescriptorSetLayout(s.device, s.setLayout, nullptr);
        if (s.frameDataSetLayout != VK_NULL_HANDLE)
            vkDestroyDescriptorSetLayout(s.device, s.frameDataSetLayout, nullptr);
        s.destroyAttachments();
        for (VkPipeline pipeline : s.pipelines)
            if (pipeline != VK_NULL_HANDLE)
                vkDestroyPipeline(s.device, pipeline, nullptr);
        if (s.vertexShader != VK_NULL_HANDLE)
            vkDestroyShaderModule(s.device, s.vertexShader, nullptr);
        if (s.fragmentShader != VK_NULL_HANDLE)
            vkDestroyShaderModule(s.device, s.fragmentShader, nullptr);
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

        // Textures upload before the pass begins.
        std::vector<const GpuTexture *> drawTextures(frame.draws.size(), nullptr);
        for (size_t i = 0; i < frame.draws.size(); ++i)
        {
            const std::shared_ptr<const TextureData> &data =
                frame.draws[i].material.texture ? frame.draws[i].material.texture : s.white;
            drawTextures[i] = s.gpuTexture(cmd, data, serial);
        }

        // Every skinned draw's four bones and every lit draw's lighting
        // block, in draw order.
        FrameDataSlot &data = s.frameData[serial % kFrameSlots];
        std::vector<int32_t> boneBases(frame.draws.size(), -1);
        std::vector<int32_t> lightBases(frame.draws.size(), -1);
        std::vector<uint32_t> colorModes(frame.draws.size(), kColorVertex);
        uint32_t used = 0;
        for (size_t i = 0; i < frame.draws.size(); ++i)
        {
            const DrawCall &draw = frame.draws[i];
            if (draw.skinned)
            {
                boneBases[i] = static_cast<int32_t>(used);
                used += milo::mesh::kBoneCount * 4u;
            }
            colorModes[i] = colorMode(draw);
            if (colorModes[i] == kColorAmbient || colorModes[i] == kColorDirectional || colorModes[i] == kColorPoint)
            {
                lightBases[i] = static_cast<int32_t>(used);
                used += kLightingVec4s;
            }
        }
        if (!s.growFrameData(data, used))
            return false;
        for (size_t i = 0; i < frame.draws.size(); ++i)
        {
            if (boneBases[i] >= 0)
                std::memcpy(data.mapped + boneBases[i], frame.draws[i].bones.data(), sizeof(frame.draws[i].bones));
            if (lightBases[i] >= 0)
                writeLighting(frame.draws[i], data.mapped + lightBases[i]);
        }
        vmaFlushAllocation(s.allocator, data.memory, 0, VK_WHOLE_SIZE);

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
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s.layout, 1, 1, &data.set, 0, nullptr);

        std::vector<Matrix> viewProjections;
        viewProjections.reserve(frame.cameras.size());
        for (const Camera &camera : frame.cameras)
            viewProjections.push_back(viewProjection(camera, frame.yRatio));

        const float w = static_cast<float>(width);
        const float h = static_cast<float>(height);
        uint32_t boundCamera = UINT32_MAX;
        VkPipeline boundPipeline = VK_NULL_HANDLE;
        for (size_t i = 0; i < frame.draws.size(); ++i)
        {
            const DrawCall &draw = frame.draws[i];
            const GpuTexture *texture = drawTextures[i];
            if (!draw.mesh || !texture || draw.camera >= viewProjections.size())
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
            const Material &material = draw.material;
            const VkPipeline pipeline = s.pipeline(material.blend, material.zMode);
            if (pipeline == VK_NULL_HANDLE)
                continue;
            if (pipeline != boundPipeline)
            {
                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
                boundPipeline = pipeline;
            }
            PushConstants push{};
            const Matrix mvp = multiply(draw.world, viewProjections[draw.camera]);
            std::memcpy(push.mvp, mvp.data(), sizeof(push.mvp));
            std::memcpy(push.matColor, material.color, sizeof(push.matColor));
            push.flags = colorModes[i];
            if (material.prelit)
                push.flags |= kFlagPrelit;
            if (material.alphaCut)
                push.flags |= kFlagAlphaCut;
            // Intensify raises a textured pass's rgb scale from 128 to 255.
            if (material.texture && material.intensify)
                push.flags |= kFlagIntensify;
            push.boneBase = boneBases[i];
            push.lightBase = lightBases[i];
            std::memcpy(push.uvRows, material.uvXfm, sizeof(push.uvRows));
            std::memcpy(push.uvOffset, material.uvXfm + 4, sizeof(push.uvOffset));
            vkCmdPushConstants(cmd, s.layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                               sizeof(push), &push);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s.layout, 0, 1,
                                    &texture->sets[material.texWrap ? 1 : 0], 0, nullptr);
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
