#include "host/scene_renderer.h"

#include "dev/draw_dump.h"
#include "host/mesh_push.h"
#include "milo/layout.h"
#include "render/camera.h"
#include "settings/settings.h"

#include <vk_mem_alloc.h>

#include "mesh_frag.h"
#include "mesh_vert.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iostream>
#include <unordered_map>

namespace gh2
{
    namespace
    {
        constexpr VkFormat kColorFormat = VK_FORMAT_R8G8B8A8_UNORM;
        // Frames a GPU resource must outlive its last use by.
        constexpr uint64_t kRetireAfter = 3;

        // Each frame writes its bones and lighting to its own slot's buffer,
        // so a slot is reused only after its frame has retired.
        constexpr uint32_t kFrameSlots = kRetireAfter + 1u;
        constexpr uint32_t kMinFrameData = 1024u; // vec4s
        // A draw's lighting block: ambient, three light colours, then the
        // three directions to the lights in the mesh's own space.
        constexpr uint32_t kLightingVec4s = 7u;
        // A draw's environ tex gen block: see writeEnvTexGen.
        constexpr uint32_t kEnvTexGenVec4s = 4u;

        // By blend, z mode, alpha write and dest alpha test.
        constexpr uint32_t kPipelineCount =
            static_cast<uint32_t>(milo::mat::kBlendCount) * static_cast<uint32_t>(milo::mat::kZModeCount) * 4u;

        // The GS's ALPHA_1 for each blend, as PsMat::Update sets it.
        //
        // The alpha written is what a rendered texture's cards are cut by.
        // FBA sets the alpha bit of every pixel a material without
        // alpha_write draws (PsMat::Select 0x3d8498), so a blended pixel
        // keeps the larger of its alpha and what is there, and one that is
        // not blended is written as 1 (kFlagSetAlpha).
        VkPipelineColorBlendAttachmentState blendState(uint32_t blend, bool alphaWrite)
        {
            VkPipelineColorBlendAttachmentState state{};
            state.blendEnable = VK_TRUE;
            state.colorBlendOp = VK_BLEND_OP_ADD;
            state.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
            state.dstAlphaBlendFactor = alphaWrite ? VK_BLEND_FACTOR_ZERO : VK_BLEND_FACTOR_ONE;
            state.alphaBlendOp = alphaWrite ? VK_BLEND_OP_ADD : VK_BLEND_OP_MAX;
            state.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                   VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
            switch (blend)
            {
            case milo::mat::kBlendDest:
                state.srcColorBlendFactor = VK_BLEND_FACTOR_ZERO;
                state.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
                state.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
                state.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
                state.alphaBlendOp = VK_BLEND_OP_ADD;
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
        //
        // The stencil is the top bit of the frame buffer's alpha, the one a
        // 16-bit buffer has. FBA sets it on every pixel a material draws
        // unless the material has alpha_write, whose own alpha (0.99 on the
        // highway's) then leaves it clear. DATE draws only where it is clear.
        VkPipelineDepthStencilStateCreateInfo depthState(uint32_t zMode, bool alphaWrite, bool destAlphaTest)
        {
            VkPipelineDepthStencilStateCreateInfo state{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
            state.stencilTestEnable = VK_TRUE;
            state.front.failOp = VK_STENCIL_OP_KEEP;
            state.front.depthFailOp = VK_STENCIL_OP_KEEP;
            state.front.compareOp = destAlphaTest ? VK_COMPARE_OP_EQUAL : VK_COMPARE_OP_ALWAYS;
            state.front.compareMask = 1u;
            state.front.writeMask = 1u;
            // A tested pixel's bit is clear, so setting it is one up.
            state.front.reference = destAlphaTest ? 0u : 1u;
            state.front.passOp = alphaWrite      ? VK_STENCIL_OP_ZERO
                                 : destAlphaTest ? VK_STENCIL_OP_INCREMENT_AND_CLAMP
                                                 : VK_STENCIL_OP_REPLACE;
            state.back = state.front;
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

        // One image a camera drew into: a rendered RndTex as it stood after
        // one run of draws. A target drawn, sampled, then drawn again within
        // a frame is two of these, so each sampler reads the one current at
        // its place in draw order: the crowd renders each character type into
        // one sheet in turn, drawing that type's cards between.
        struct TargetImage
        {
            VkImage color = VK_NULL_HANDLE;
            VmaAllocation colorMemory = VK_NULL_HANDLE;
            VkImageView colorView = VK_NULL_HANDLE;
            // The first level alone, which is drawn into; the rest are
            // filled from it (fillTargetLevels).
            VkImageView baseView = VK_NULL_HANDLE;
            uint32_t levels = 1;
            VkImage depth = VK_NULL_HANDLE;
            VmaAllocation depthMemory = VK_NULL_HANDLE;
            VkImageView depthView = VK_NULL_HANDLE;
            // Multisampled colour, resolved into `color`; none at one sample.
            VkImage msaa = VK_NULL_HANDLE;
            VmaAllocation msaaMemory = VK_NULL_HANDLE;
            VkImageView msaaView = VK_NULL_HANDLE;
            VkFramebuffer framebuffer = VK_NULL_HANDLE;
            VkDescriptorSet sets[2]{}; // clamped, wrapped
            uint32_t width = 0;
            uint32_t height = 0;
            bool copied = false; // filled by a screen copy: colour only
        };

        // Room for this many live textures before the pool refuses.
        constexpr uint32_t kMaxTextures = 4096;

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
            if (draw.environment.kind == Environ::kPoint)
                return kColorPoint;
            return draw.environment.kind == Environ::kDirectional ? kColorDirectional : kColorAmbient;
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
            const Environ &e = draw.environment;
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

        // Program 0x139, the environ tex gen (0x09c8..0x0bd8), takes the vert
        // and its normal through the lighting matrix W (qw676..679) and then
        // the material's rows M (qw691..693), the eye (qw698) through M alone,
        // and reflects the vert-to-eye vector about the normal:
        //   v = E.M - (p.W).M,  n' = n.W.M,  r = 2n'(n'.v) - v
        //   uv = r.xy * 0.5 / |v| + (0.5, 0.5)
        // The block holds W.M as three rows, then (E - W's position).M, so
        // v = [3] - p.[0..2].
        void writeEnvTexGen(const DrawCall &draw, const float eye[3], Vec4 *out)
        {
            const Matrix &w = draw.lightWorld;
            const float(&m)[3][3] = draw.material.envRows;
            for (uint32_t r = 0; r < 3; ++r)
            {
                out[r] = {{0.0f, 0.0f, 0.0f, 0.0f}};
                for (uint32_t c = 0; c < 3; ++c)
                    out[r].v[c] = w[r * 4 + 0] * m[0][c] + w[r * 4 + 1] * m[1][c] + w[r * 4 + 2] * m[2][c];
            }
            out[3] = {{0.0f, 0.0f, 0.0f, 0.0f}};
            for (uint32_t c = 0; c < 3; ++c)
                for (uint32_t k = 0; k < 3; ++k)
                    out[3].v[c] += (eye[k] - w[12 + k]) * m[k][c];
        }

        // Program 0x410, the projected tex gen (0x2080..0x21b0), takes the
        // vert through the lighting matrix W (qw676..679) and the material's
        // rows and offset M (qw691..694), and keeps x and y:
        //   uv = ((p.W).M).xy
        // The block holds W.M as three rows, then W's position through M.
        void writeProjTexGen(const DrawCall &draw, Vec4 *out)
        {
            const Matrix &w = draw.lightWorld;
            const float(&m)[4][3] = draw.material.projRows;
            for (uint32_t r = 0; r < 4; ++r)
            {
                out[r] = {{0.0f, 0.0f, 0.0f, 0.0f}};
                for (uint32_t c = 0; c < 3; ++c)
                    out[r].v[c] = w[r * 4 + 0] * m[0][c] + w[r * 4 + 1] * m[1][c] + w[r * 4 + 2] * m[2][c] +
                                  (r == 3 ? m[3][c] : 0.0f);
            }
        }

        // Program 0x347, the sphere tex gen (0x347..0x368), takes the normal
        // through the lighting matrix's rotation W (qw676..678) and the rows
        // G (qw691..693), keeps x and y and adds the offset (qw694):
        //   uv = (n.W.G).xy + offset
        // The block holds W.G as three rows, then the offset.
        void writeSphereTexGen(const DrawCall &draw, Vec4 *out)
        {
            const Matrix &w = draw.lightWorld;
            const float(&g)[4][3] = draw.material.sphereRows;
            for (uint32_t r = 0; r < 3; ++r)
            {
                out[r] = {{0.0f, 0.0f, 0.0f, 0.0f}};
                for (uint32_t c = 0; c < 3; ++c)
                    out[r].v[c] = w[r * 4 + 0] * g[0][c] + w[r * 4 + 1] * g[1][c] + w[r * 4 + 2] * g[2][c];
            }
            out[3] = {{g[3][0], g[3][1], g[3][2], 0.0f}};
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
        // Depth with a stencil: 32-bit float where the device has it, which
        // Vulkan promises of one of these two.
        VkFormat depthFormat = VK_FORMAT_D32_SFLOAT_S8_UINT;
        VkRenderPass renderPass = VK_NULL_HANDLE;
        // The main pass in parts, around a screen copy: up to the first, and
        // from each on. Same attachments, so its framebuffer and pipelines.
        VkRenderPass firstPass = VK_NULL_HANDLE;
        VkRenderPass resumePass = VK_NULL_HANDLE;
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
        VkImage msaa = VK_NULL_HANDLE; // resolved into the target; none at one sample
        VmaAllocation msaaMemory = VK_NULL_HANDLE;
        VkImageView msaaView = VK_NULL_HANDLE;
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

        // Rendered textures by RndTex, one image per generation within a
        // frame, kept for the run. Their pass has the main pass's formats, so
        // every pipeline draws into either.
        VkRenderPass targetPass = VK_NULL_HANDLE;
        std::unordered_map<uint32_t, std::vector<TargetImage>> targets;

        // Samples per pixel for every pass, from the msaa setting.
        VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;

        // The main pass and the target pass, alike but for the finished
        // image's layout, so every pipeline draws into either: passes are
        // only compatible when their attachments and dependencies match.
        // Colour and depth are multisampled when `samples` is above one and
        // resolve into the finished image, attachment 2; else colour is it.
        // The finished image is read before a pass draws into it again and
        // after: blit and readback for the main pass, samplers for a target.
        //
        // A pass stopped for a screen copy and taken up again is two: `keep`
        // stores what the next needs, `resume` loads it in place of clearing.
        bool createPass(VkImageLayout finished, bool keep, bool resume, VkRenderPass &out)
        {
            const bool resolve = samples != VK_SAMPLE_COUNT_1_BIT;
            VkAttachmentDescription attachments[3]{};
            attachments[0].format = kColorFormat;
            attachments[0].samples = samples;
            attachments[0].loadOp = resume ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_CLEAR;
            attachments[0].storeOp = resolve && !keep ? VK_ATTACHMENT_STORE_OP_DONT_CARE : VK_ATTACHMENT_STORE_OP_STORE;
            attachments[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            attachments[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
            attachments[0].finalLayout = resolve ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL : finished;
            attachments[0].initialLayout = resume ? attachments[0].finalLayout : VK_IMAGE_LAYOUT_UNDEFINED;
            attachments[1] = attachments[0];
            attachments[1].format = depthFormat;
            attachments[1].storeOp = keep ? VK_ATTACHMENT_STORE_OP_STORE : VK_ATTACHMENT_STORE_OP_DONT_CARE;
            attachments[1].stencilLoadOp = attachments[1].loadOp;
            attachments[1].stencilStoreOp = attachments[1].storeOp;
            attachments[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
            attachments[1].initialLayout = resume ? attachments[1].finalLayout : VK_IMAGE_LAYOUT_UNDEFINED;
            attachments[2] = attachments[0];
            attachments[2].samples = VK_SAMPLE_COUNT_1_BIT;
            attachments[2].loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            attachments[2].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            attachments[2].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            attachments[2].finalLayout = finished;

            VkAttachmentReference colorRef{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
            VkAttachmentReference depthRef{1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
            VkAttachmentReference resolveRef{2, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
            VkSubpassDescription subpass{};
            subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
            subpass.colorAttachmentCount = 1;
            subpass.pColorAttachments = &colorRef;
            subpass.pResolveAttachments = resolve ? &resolveRef : nullptr;
            subpass.pDepthStencilAttachment = &depthRef;

            VkSubpassDependency before{};
            before.srcSubpass = VK_SUBPASS_EXTERNAL;
            before.dstSubpass = 0;
            before.srcStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                                  VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
            before.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                                  VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
            before.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_SHADER_READ_BIT |
                                   VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
            before.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                                   VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
            VkSubpassDependency after{};
            after.srcSubpass = 0;
            after.dstSubpass = VK_SUBPASS_EXTERNAL;
            after.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
            after.dstStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
            after.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            after.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_SHADER_READ_BIT;
            const VkSubpassDependency dependencies[] = {before, after};

            VkRenderPassCreateInfo info{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
            info.attachmentCount = resolve ? 3u : 2u;
            info.pAttachments = attachments;
            info.subpassCount = 1;
            info.pSubpasses = &subpass;
            info.dependencyCount = 2;
            info.pDependencies = dependencies;
            return check(vkCreateRenderPass(device, &info, nullptr, &out), "render pass");
        }

        // A colour or depth attachment at `samples` that no one samples:
        // depth, and colour that resolves into another image. `transient`
        // when nothing outlives its pass, which the main pass's must to be
        // taken up again after a screen copy.
        bool createAttachment(VkFormat format, VkImageUsageFlags usage, bool transient, uint32_t w, uint32_t h,
                              VkImage &image, VmaAllocation &memory, VkImageView &view)
        {
            VkImageCreateInfo imageInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
            imageInfo.imageType = VK_IMAGE_TYPE_2D;
            imageInfo.format = format;
            imageInfo.extent = {w, h, 1};
            imageInfo.mipLevels = 1;
            imageInfo.arrayLayers = 1;
            imageInfo.samples = samples;
            imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
            imageInfo.usage = usage | (transient ? VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT : 0u);
            VmaAllocationCreateInfo alloc{};
            alloc.usage = VMA_MEMORY_USAGE_AUTO;
            if (!check(vmaCreateImage(allocator, &imageInfo, &alloc, &image, &memory, nullptr), "attachment"))
                return false;
            VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
            viewInfo.image = image;
            viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
            viewInfo.format = format;
            const VkImageAspectFlags aspect = format == depthFormat
                                                  ? VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT
                                                  : VK_IMAGE_ASPECT_COLOR_BIT;
            viewInfo.subresourceRange = {aspect, 0, 1, 0, 1};
            return check(vkCreateImageView(device, &viewInfo, nullptr, &view), "attachment view");
        }

        void destroyTarget(TargetImage &t)
        {
            if (t.sets[0] != VK_NULL_HANDLE)
                vkFreeDescriptorSets(device, pool, 2, t.sets);
            if (t.framebuffer != VK_NULL_HANDLE)
                vkDestroyFramebuffer(device, t.framebuffer, nullptr);
            if (t.colorView != VK_NULL_HANDLE)
                vkDestroyImageView(device, t.colorView, nullptr);
            if (t.baseView != VK_NULL_HANDLE)
                vkDestroyImageView(device, t.baseView, nullptr);
            if (t.depthView != VK_NULL_HANDLE)
                vkDestroyImageView(device, t.depthView, nullptr);
            if (t.msaaView != VK_NULL_HANDLE)
                vkDestroyImageView(device, t.msaaView, nullptr);
            if (t.color != VK_NULL_HANDLE)
                vmaDestroyImage(allocator, t.color, t.colorMemory);
            if (t.depth != VK_NULL_HANDLE)
                vmaDestroyImage(allocator, t.depth, t.depthMemory);
            if (t.msaa != VK_NULL_HANDLE)
                vmaDestroyImage(allocator, t.msaa, t.msaaMemory);
            t = TargetImage{};
        }

        // Fills a copied image from the main target, which the pass just
        // ended left in TRANSFER_SRC_OPTIMAL, taking its first pixel from x
        // and y. What falls outside the picture is black.
        void copyFromTarget(VkCommandBuffer cmd, const TargetImage &to, int32_t x, int32_t y)
        {
            VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
            barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.image = to.color;
            barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr,
                                 0, nullptr, 1, &barrier);

            const int32_t x0 = std::max(x, 0);
            const int32_t y0 = std::max(y, 0);
            const int32_t x1 = std::min(x + static_cast<int32_t>(to.width), static_cast<int32_t>(width));
            const int32_t y1 = std::min(y + static_cast<int32_t>(to.height), static_cast<int32_t>(height));
            const bool any = x1 > x0 && y1 > y0;
            if (!any || static_cast<uint32_t>(x1 - x0) != to.width || static_cast<uint32_t>(y1 - y0) != to.height)
            {
                const VkClearColorValue black{};
                vkCmdClearColorImage(cmd, to.color, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1,
                                     &barrier.subresourceRange);
            }
            if (any)
            {
                VkImageCopy region{};
                region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                region.srcOffset = {x0, y0, 0};
                region.dstSubresource = region.srcSubresource;
                region.dstOffset = {x0 - x, y0 - y, 0};
                region.extent = {static_cast<uint32_t>(x1 - x0), static_cast<uint32_t>(y1 - y0), 1};
                vkCmdCopyImage(cmd, target, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, to.color,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
            }

            barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0,
                                 nullptr, 0, nullptr, 1, &barrier);
        }

        // Fills a drawn target's smaller levels, each from the one above. The
        // pass just ended left the first in SHADER_READ_ONLY_OPTIMAL.
        void fillTargetLevels(VkCommandBuffer cmd, const TargetImage &t)
        {
            VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
            barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.image = t.color;
            auto move = [&](uint32_t level, VkImageLayout from, VkImageLayout to, VkAccessFlags was, VkAccessFlags is,
                            VkPipelineStageFlags before, VkPipelineStageFlags after)
            {
                barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, level, 1, 0, 1};
                barrier.oldLayout = from;
                barrier.newLayout = to;
                barrier.srcAccessMask = was;
                barrier.dstAccessMask = is;
                vkCmdPipelineBarrier(cmd, before, after, 0, 0, nullptr, 0, nullptr, 1, &barrier);
            };
            for (uint32_t level = 1; level < t.levels; ++level)
            {
                move(level - 1u, level == 1u ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                     VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                     level == 1u ? VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT : VK_ACCESS_TRANSFER_WRITE_BIT,
                     VK_ACCESS_TRANSFER_READ_BIT,
                     level == 1u ? VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT : VK_PIPELINE_STAGE_TRANSFER_BIT,
                     VK_PIPELINE_STAGE_TRANSFER_BIT);
                move(level, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                     VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
                VkImageBlit blit{};
                blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level - 1u, 0, 1};
                blit.srcOffsets[1] = {static_cast<int32_t>(std::max(t.width >> (level - 1u), 1u)),
                                      static_cast<int32_t>(std::max(t.height >> (level - 1u), 1u)), 1};
                blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, 0, 1};
                blit.dstOffsets[1] = {static_cast<int32_t>(std::max(t.width >> level, 1u)),
                                      static_cast<int32_t>(std::max(t.height >> level, 1u)), 1};
                vkCmdBlitImage(cmd, t.color, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, t.color,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
                move(level - 1u, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                     VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                     VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
            }
            if (t.levels > 1u)
                move(t.levels - 1u, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                     VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                     VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
        }

        // The image for a target's generation, made or remade at its size.
        // `copied` is one a screen copy fills: nothing draws into it.
        TargetImage *targetImage(uint32_t tex, uint32_t generation, uint32_t w, uint32_t h, bool copied)
        {
            std::vector<TargetImage> &images = targets[tex];
            if (images.size() <= generation)
                images.resize(generation + 1u);
            TargetImage &t = images[generation];
            // Retail fills three smaller levels of a rendered texture once
            // it is drawn (PsTex::FinishDrawTarget 0x1a0a38), which the
            // crowd's cards are sampled from at a distance.
            uint32_t levels = 1;
            if (!copied && settings::get(settings::kMipmaps))
                while (levels < 4u && (std::min(w, h) >> levels) != 0u)
                    ++levels;
            if (t.color != VK_NULL_HANDLE && t.width == w && t.height == h && t.copied == copied && t.levels == levels)
                return &t;
            if (t.color != VK_NULL_HANDLE)
            {
                vkDeviceWaitIdle(device);
                destroyTarget(t);
            }
            t.copied = copied;
            t.levels = levels;

            VkImageCreateInfo imageInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
            imageInfo.imageType = VK_IMAGE_TYPE_2D;
            imageInfo.format = kColorFormat;
            imageInfo.extent = {w, h, 1};
            imageInfo.mipLevels = levels;
            imageInfo.arrayLayers = 1;
            imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
            imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
            imageInfo.usage = (copied ? VK_IMAGE_USAGE_TRANSFER_DST_BIT : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) |
                              VK_IMAGE_USAGE_SAMPLED_BIT;
            if (levels > 1u)
                imageInfo.usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
            VmaAllocationCreateInfo alloc{};
            alloc.usage = VMA_MEMORY_USAGE_AUTO;
            if (!check(vmaCreateImage(allocator, &imageInfo, &alloc, &t.color, &t.colorMemory, nullptr), "target"))
                return nullptr;
            VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
            view.image = t.color;
            view.viewType = VK_IMAGE_VIEW_TYPE_2D;
            view.format = kColorFormat;
            // The GS's copy carries the frame buffer's alpha, whose top bit
            // every material but the highway's own sets (FBA, PsMat::Select
            // 0x3d8498) and the clear sets too. The picture's alpha here is
            // the last fragment's, so a copy reads as opaque instead.
            if (copied)
                view.components.a = VK_COMPONENT_SWIZZLE_ONE;
            view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, levels, 0, 1};
            bool ok = check(vkCreateImageView(device, &view, nullptr, &t.colorView), "target view");
            view.subresourceRange.levelCount = 1;
            ok = ok && check(vkCreateImageView(device, &view, nullptr, &t.baseView), "target base view");

            const bool resolve = samples != VK_SAMPLE_COUNT_1_BIT;
            if (resolve && !copied)
                ok = ok && createAttachment(kColorFormat, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, true, w, h, t.msaa,
                                            t.msaaMemory, t.msaaView);
            if (!copied)
                ok = ok && createAttachment(depthFormat, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, true, w, h,
                                            t.depth, t.depthMemory, t.depthView);
            if (ok && !copied)
            {
                const VkImageView plain[] = {t.baseView, t.depthView};
                const VkImageView resolved[] = {t.msaaView, t.depthView, t.baseView};
                VkFramebufferCreateInfo fb{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
                fb.renderPass = targetPass;
                fb.attachmentCount = resolve ? 3u : 2u;
                fb.pAttachments = resolve ? resolved : plain;
                fb.width = w;
                fb.height = h;
                fb.layers = 1;
                ok = check(vkCreateFramebuffer(device, &fb, nullptr, &t.framebuffer), "target framebuffer");
            }
            if (ok)
            {
                const VkDescriptorSetLayout layouts[2] = {setLayout, setLayout};
                VkDescriptorSetAllocateInfo setInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
                setInfo.descriptorPool = pool;
                setInfo.descriptorSetCount = 2;
                setInfo.pSetLayouts = layouts;
                ok = check(vkAllocateDescriptorSets(device, &setInfo, t.sets), "target sets");
            }
            if (!ok)
            {
                destroyTarget(t);
                return nullptr;
            }
            for (int wrap = 0; wrap < 2; ++wrap)
            {
                VkDescriptorImageInfo image{samplers[wrap], t.colorView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
                VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
                write.dstSet = t.sets[wrap];
                write.descriptorCount = 1;
                write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                write.pImageInfo = &image;
                vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
            }
            t.width = w;
            t.height = h;
            return &t;
        }

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

            // PsTex's TEX1: bilinear, and for a texture with mips (RndTex
            // +0x6c) nearest mip (MMIN 4, SyncBitmap 0x1a1a94). The GS picks
            // the level by distance and a per-texture K; derivatives here.
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
                info.maxLod = VK_LOD_CLAMP_NONE;
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
            // The game's levels are uploaded; with the mipmaps setting, the
            // rest down to 1x1 are blitted from the smallest of them, fonts
            // aside.
            const uint32_t stored = 1u + static_cast<uint32_t>(data->mips.size());
            uint32_t levels = stored;
            if (settings::get(settings::kMipmaps) && !data->text)
                while ((std::max(data->width, data->height) >> (levels - 1u)) > 1u)
                    ++levels;
            std::vector<uint8_t> staged = data->rgba;
            for (const std::vector<uint8_t> &mip : data->mips)
                staged.insert(staged.end(), mip.begin(), mip.end());
            if (!createBuffer(staged.data(), staged.size(), VK_BUFFER_USAGE_TRANSFER_SRC_BIT, gpu.staging,
                              gpu.stagingMemory))
                return nullptr;

            VkImageCreateInfo imageInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
            imageInfo.imageType = VK_IMAGE_TYPE_2D;
            imageInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
            imageInfo.extent = {data->width, data->height, 1};
            imageInfo.mipLevels = levels;
            imageInfo.arrayLayers = 1;
            imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
            imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
            imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                              VK_IMAGE_USAGE_SAMPLED_BIT;
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
            view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, levels, 0, 1};
            if (!check(vkCreateImageView(device, &view, nullptr, &gpu.view), "texture view"))
            {
                vmaDestroyImage(allocator, gpu.image, gpu.memory);
                vmaDestroyBuffer(allocator, gpu.staging, gpu.stagingMemory);
                return nullptr;
            }

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
            barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, levels, 0, 1};
            barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0,
                                 nullptr, 0, nullptr, 1, &barrier);
            std::vector<VkBufferImageCopy> copies(stored);
            VkDeviceSize offset = 0;
            for (uint32_t level = 0; level < stored; ++level)
            {
                copies[level].bufferOffset = offset;
                copies[level].imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, 0, 1};
                copies[level].imageExtent = {std::max(data->width >> level, 1u), std::max(data->height >> level, 1u),
                                             1};
                offset += (level == 0 ? data->rgba : data->mips[level - 1u]).size();
            }
            vkCmdCopyBufferToImage(cmd, gpu.staging, gpu.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, stored,
                                   copies.data());

            // Each level is final once the next is filled: blitted levels read
            // the one above as a transfer source first.
            auto toShaderRead = [&](uint32_t level, VkImageLayout from, VkAccessFlags access)
            {
                barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, level, 1, 0, 1};
                barrier.oldLayout = from;
                barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                barrier.srcAccessMask = access;
                barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
                vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0,
                                     nullptr, 0, nullptr, 1, &barrier);
            };
            for (uint32_t level = 1; level < levels; ++level)
            {
                if (level < stored)
                {
                    toShaderRead(level - 1u, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT);
                    continue;
                }
                barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, level - 1u, 1, 0, 1};
                barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
                barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
                vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0,
                                     nullptr, 0, nullptr, 1, &barrier);
                VkImageBlit blit{};
                blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level - 1u, 0, 1};
                blit.srcOffsets[1] = {static_cast<int32_t>(std::max(data->width >> (level - 1u), 1u)),
                                      static_cast<int32_t>(std::max(data->height >> (level - 1u), 1u)), 1};
                blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, 0, 1};
                blit.dstOffsets[1] = {static_cast<int32_t>(std::max(data->width >> level, 1u)),
                                      static_cast<int32_t>(std::max(data->height >> level, 1u)), 1};
                vkCmdBlitImage(cmd, gpu.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, gpu.image,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
                toShaderRead(level - 1u, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_TRANSFER_READ_BIT);
            }
            toShaderRead(levels - 1u, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT);

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

        VkPipeline pipeline(uint32_t blend, uint32_t zMode, bool alphaWrite, bool destAlphaTest)
        {
            VkPipeline &slot = pipelines[((blend * milo::mat::kZModeCount + zMode) * 2u + (alphaWrite ? 1u : 0u)) * 2u +
                                         (destAlphaTest ? 1u : 0u)];
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

            // Captured vertices upload as they are.
            VkVertexInputBindingDescription binding{0, sizeof(Vertex), VK_VERTEX_INPUT_RATE_VERTEX};
            VkVertexInputAttributeDescription attributes[4] = {
                {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, pos)},
                {1, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(Vertex, color)},
                {2, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(Vertex, uv)},
                {3, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, normal)},
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
            multisample.rasterizationSamples = samples;

            const VkPipelineDepthStencilStateCreateInfo depth = depthState(zMode, alphaWrite, destAlphaTest);

            const VkPipelineColorBlendAttachmentState attachment = blendState(blend, alphaWrite);
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
            if (msaaView != VK_NULL_HANDLE)
                vkDestroyImageView(device, msaaView, nullptr);
            if (msaa != VK_NULL_HANDLE)
                vmaDestroyImage(allocator, msaa, msaaMemory);
            framebuffer = VK_NULL_HANDLE;
            targetView = VK_NULL_HANDLE;
            depthView = VK_NULL_HANDLE;
            depth = VK_NULL_HANDLE;
            msaaView = VK_NULL_HANDLE;
            msaa = VK_NULL_HANDLE;
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

            const bool resolve = samples != VK_SAMPLE_COUNT_1_BIT;
            if (resolve && !createAttachment(kColorFormat, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, false, w, h, msaa,
                                             msaaMemory, msaaView))
                return false;
            if (!createAttachment(depthFormat, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, false, w, h, depth,
                                  depthMemory, depthView))
                return false;

            const VkImageView plain[] = {targetView, depthView};
            const VkImageView resolved[] = {msaaView, depthView, targetView};
            VkFramebufferCreateInfo fb{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
            fb.renderPass = renderPass;
            fb.attachmentCount = resolve ? 3u : 2u;
            fb.pAttachments = resolve ? resolved : plain;
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
                if (mesh->verts.empty() || mesh->indices.empty())
                    return nullptr;
                GpuMesh gpu;
                gpu.source = mesh;
                if (!createBuffer(mesh->verts.data(), mesh->verts.size() * sizeof(Vertex),
                                  VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, gpu.vertices, gpu.vertexMemory) ||
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

    bool SceneRenderer::initialize(VkDevice device, VmaAllocator allocator, VkSampleCountFlagBits samples)
    {
        m_state = std::make_unique<State>();
        m_state->device = device;
        m_state->allocator = allocator;
        m_state->samples = samples;
        VmaAllocatorInfo allocatorInfo{};
        vmaGetAllocatorInfo(allocator, &allocatorInfo);
        VkFormatProperties depthProperties{};
        vkGetPhysicalDeviceFormatProperties(allocatorInfo.physicalDevice, m_state->depthFormat, &depthProperties);
        if ((depthProperties.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) == 0u)
            m_state->depthFormat = VK_FORMAT_D24_UNORM_S8_UINT;
        return m_state->createPass(VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, false, false, m_state->renderPass) &&
               m_state->createPass(VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, true, false, m_state->firstPass) &&
               m_state->createPass(VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, true, true, m_state->resumePass) &&
               m_state->createPass(VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, false, false, m_state->targetPass) &&
               m_state->createDescriptorState() && m_state->createLayout();
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
        for (auto &entry : s.targets)
            for (TargetImage &image : entry.second)
                s.destroyTarget(image);
        s.targets.clear();
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
        for (VkRenderPass pass : {s.renderPass, s.firstPass, s.resumePass})
            if (pass != VK_NULL_HANDLE)
                vkDestroyRenderPass(s.device, pass, nullptr);
        if (s.targetPass != VK_NULL_HANDLE)
            vkDestroyRenderPass(s.device, s.targetPass, nullptr);
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

        // Every skinned draw's four bones, every lit draw's lighting block
        // and every environ tex gen's block, in draw order.
        FrameDataSlot &data = s.frameData[serial % kFrameSlots];
        std::vector<int32_t> boneBases(frame.draws.size(), -1);
        std::vector<int32_t> lightBases(frame.draws.size(), -1);
        std::vector<int32_t> envBases(frame.draws.size(), -1);
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
            if (draw.material.texGen == milo::mat::kTexGenEnviron ||
                draw.material.texGen == milo::mat::kTexGenProjected || draw.material.sphere)
            {
                envBases[i] = static_cast<int32_t>(used);
                used += kEnvTexGenVec4s;
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
            if (envBases[i] >= 0 && frame.draws[i].material.sphere)
                writeSphereTexGen(frame.draws[i], data.mapped + envBases[i]);
            else if (envBases[i] >= 0 && frame.draws[i].material.texGen == milo::mat::kTexGenProjected)
                writeProjTexGen(frame.draws[i], data.mapped + envBases[i]);
            else if (envBases[i] >= 0)
                writeEnvTexGen(frame.draws[i], frame.cameras[frame.draws[i].camera].eye, data.mapped + envBases[i]);
        }
        vmaFlushAllocation(s.allocator, data.memory, 0, VK_WHOLE_SIZE);

        // Which pass each draw goes into and which target image it samples.
        // A draw through a camera with a target opens that target's next
        // generation when the current one has been sampled since it was drawn.
        struct TargetRun
        {
            uint32_t tex;
            uint32_t generation;
            uint32_t width;
            uint32_t height;
            std::vector<size_t> draws;
            TargetImage *image = nullptr;
            // For a screen copy, which no draw goes into: where its texture's
            // first pixel is in the picture, and the draw it is made before.
            bool copied = false;
            int32_t x = 0;
            int32_t y = 0;
            size_t before = 0;
        };
        std::vector<TargetRun> runs;
        std::vector<int32_t> drawRun(frame.draws.size(), -1);
        std::vector<int32_t> sampleRun(frame.draws.size(), -1);
        std::vector<size_t> copyRuns; // in the order they are made
        {
            std::unordered_map<uint32_t, int32_t> current;
            std::unordered_map<uint32_t, bool> sampledSince;
            std::unordered_map<uint32_t, uint32_t> generations;
            // A copy is the size its texture covers of the picture as drawn
            // here, so the pixels it puts back are the ones it took.
            const float sx = static_cast<float>(width) / static_cast<float>(frame.width);
            const float sy = static_cast<float>(height) / static_cast<float>(frame.height);
            size_t nextCopy = 0;
            for (size_t i = 0; i < frame.draws.size(); ++i)
            {
                for (; nextCopy < frame.copies.size() && frame.copies[nextCopy].before <= i; ++nextCopy)
                {
                    const Frame::ScreenCopy &copy = frame.copies[nextCopy];
                    const int32_t x0 = static_cast<int32_t>(std::lround(static_cast<float>(copy.x) * sx));
                    const int32_t y0 = static_cast<int32_t>(std::lround(static_cast<float>(copy.y) * sy));
                    const int32_t x1 =
                        static_cast<int32_t>(std::lround(static_cast<float>(copy.x + static_cast<int32_t>(copy.width)) * sx));
                    const int32_t y1 =
                        static_cast<int32_t>(std::lround(static_cast<float>(copy.y + static_cast<int32_t>(copy.height)) * sy));
                    if (x1 <= x0 || y1 <= y0)
                        continue;
                    TargetRun run{copy.tex, generations[copy.tex]++, static_cast<uint32_t>(x1 - x0),
                                  static_cast<uint32_t>(y1 - y0), {}};
                    run.copied = true;
                    run.x = x0;
                    run.y = y0;
                    run.before = i;
                    runs.push_back(run);
                    copyRuns.push_back(runs.size() - 1u);
                    current.insert_or_assign(copy.tex, static_cast<int32_t>(runs.size() - 1u));
                    sampledSince[copy.tex] = false;
                }
                const DrawCall &draw = frame.draws[i];
                if (draw.camera >= frame.cameras.size())
                    continue;
                const uint32_t rt = draw.material.renderTarget;
                if (rt != 0u)
                {
                    const auto found = current.find(rt);
                    if (found != current.end())
                    {
                        sampleRun[i] = found->second;
                        sampledSince[rt] = true;
                    }
                }
                const Camera &camera = frame.cameras[draw.camera];
                if (camera.target == 0u)
                    continue;
                if (camera.targetWidth == 0u || camera.targetHeight == 0u)
                {
                    drawRun[i] = INT32_MAX; // into a target with no size: nowhere
                    continue;
                }
                auto found = current.find(camera.target);
                if (found == current.end() || sampledSince[camera.target])
                {
                    runs.push_back({camera.target, generations[camera.target]++, camera.targetWidth,
                                    camera.targetHeight, {}});
                    found = current.insert_or_assign(camera.target, static_cast<int32_t>(runs.size() - 1u)).first;
                    sampledSince[camera.target] = false;
                }
                drawRun[i] = found->second;
                runs[found->second].draws.push_back(i);
            }
        }
        // Each target's generations are sized before any is taken, since
        // growing the list would move the ones already handed out.
        for (const TargetRun &run : runs)
        {
            std::vector<TargetImage> &images = s.targets[run.tex];
            if (images.size() <= run.generation)
                images.resize(run.generation + 1u);
        }
        for (TargetRun &run : runs)
            run.image = s.targetImage(run.tex, run.generation, run.width, run.height, run.copied);

        std::vector<Matrix> viewProjections;
        viewProjections.reserve(frame.cameras.size());
        for (const Camera &camera : frame.cameras)
            viewProjections.push_back(viewProjection(camera, frame.yRatio));

        std::vector<PushConstants> pushes(frame.draws.size());
        for (size_t i = 0; i < frame.draws.size(); ++i)
        {
            const DrawCall &draw = frame.draws[i];
            if (draw.camera >= viewProjections.size())
                continue;
            const Material &material = draw.material;
            PushConstants &push = pushes[i];
            const Matrix mvp = draw.screen ? draw.world : multiply(draw.world, viewProjections[draw.camera]);
            std::memcpy(push.mvp, mvp.data(), sizeof(push.mvp));
            std::memcpy(push.matColor, material.color, sizeof(push.matColor));
            push.flags = colorModes[i];
            if (material.prelit)
                push.flags |= kFlagPrelit;
            if (material.vertDyn)
                push.flags |= kFlagVertDyn;
            // A src-alpha blend without alpha_cut tests alpha the same way but
            // still writes Z where it fails (TEST 0x200d, PsMat::Update
            // 0x19d10c), leaving colour and the alpha bit alone. With no Z to
            // write that is alpha_cut's discard. With Z to write the fragment
            // is kept: its alpha of 0 leaves the colour, but it sets or
            // clears the bit where retail leaves it.
            const bool noZWrite = material.zMode == milo::mat::kZDisable || material.zMode == milo::mat::kZTransparent;
            if (material.alphaCut || (material.blend == milo::mat::kBlendSrcAlpha && noZWrite))
                push.flags |= kFlagAlphaCut;
            if (material.blend == milo::mat::kBlendSrc && !material.alphaWrite)
                push.flags |= kFlagSetAlpha;
            // Intensify raises a textured pass's rgb scale from 128 to 255.
            if ((material.texture || material.renderTarget != 0u) && material.intensify)
                push.flags |= kFlagIntensify;
            push.flags |= (draw.skinBones - 1u) << kSkinBonesShift;
            if (material.highlight)
                push.flags |= kFlagHighlight;
            if (material.decal)
                push.flags |= kFlagDecal;
            push.boneBase = boneBases[i];
            push.lightBase = lightBases[i];
            push.envBase = envBases[i];
            if (material.sphere)
                push.flags |= kFlagSphere;
            if (material.spread)
                push.flags |= kFlagSpread;
            if (material.texGen == milo::mat::kTexGenProjected)
                push.flags |= kFlagProjected;
            std::memcpy(push.uvRows, material.uvXfm, sizeof(push.uvRows));
            std::memcpy(push.uvOffset, material.uvXfm + 4, sizeof(push.uvOffset));
        }
        writeDrawDump(frame, pushes.data(), data.mapped);

        // Bound state outlives a render pass, so the frame data is bound once.
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s.layout, 1, 1, &data.set, 0, nullptr);
        uint32_t boundCamera = UINT32_MAX;
        VkPipeline boundPipeline = VK_NULL_HANDLE;
        // One draw into whichever pass is open, `w` by `h`.
        const auto drawOne = [&](size_t i, float w, float h) {
            const DrawCall &draw = frame.draws[i];
            const VkDescriptorSet *sets = drawTextures[i] ? drawTextures[i]->sets : nullptr;
            if (draw.material.renderTarget != 0u)
            {
                // A rendered texture nothing drew this frame reads as nothing.
                const TargetImage *image = sampleRun[i] >= 0 ? runs[sampleRun[i]].image : nullptr;
                sets = image ? image->sets : nullptr;
            }
            if (!draw.mesh || !sets || draw.camera >= viewProjections.size())
                return;
            if (frame.cameras[draw.camera].rect[2] <= 0.0f || frame.cameras[draw.camera].rect[3] <= 0.0f)
                return;
            GpuMesh *mesh = s.gpuMesh(draw.mesh, serial);
            if (!mesh)
                return;
            if (draw.camera != boundCamera)
            {
                // The camera's rect, as PsCam::Select sets its viewport and
                // scissor (0x19c504).
                const float *rect = frame.cameras[draw.camera].rect;
                VkViewport viewport{rect[0] * w, rect[1] * h, rect[2] * w, rect[3] * h, 0.0f, 1.0f};
                const int32_t x0 = static_cast<int32_t>(std::clamp(rect[0], 0.0f, 1.0f) * w);
                const int32_t y0 = static_cast<int32_t>(std::clamp(rect[1], 0.0f, 1.0f) * h);
                const int32_t x1 = static_cast<int32_t>(std::clamp(rect[0] + rect[2], 0.0f, 1.0f) * w);
                const int32_t y1 = static_cast<int32_t>(std::clamp(rect[1] + rect[3], 0.0f, 1.0f) * h);
                VkRect2D scissor{{x0, y0}, {static_cast<uint32_t>(x1 > x0 ? x1 - x0 : 0),
                                            static_cast<uint32_t>(y1 > y0 ? y1 - y0 : 0)}};
                vkCmdSetViewport(cmd, 0, 1, &viewport);
                vkCmdSetScissor(cmd, 0, 1, &scissor);
                boundCamera = draw.camera;
            }
            const Material &material = draw.material;
            const VkPipeline pipeline =
                s.pipeline(material.blend, material.zMode, material.alphaWrite, material.destAlphaTest);
            if (pipeline == VK_NULL_HANDLE)
                return;
            if (pipeline != boundPipeline)
            {
                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
                boundPipeline = pipeline;
            }
            vkCmdPushConstants(cmd, s.layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                               sizeof(PushConstants), &pushes[i]);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s.layout, 0, 1,
                                    &sets[material.texWrap ? 1 : 0], 0, nullptr);
            const VkDeviceSize offset = 0;
            vkCmdBindVertexBuffers(cmd, 0, 1, &mesh->vertices, &offset);
            vkCmdBindIndexBuffer(cmd, mesh->indices, 0, VK_INDEX_TYPE_UINT16);
            vkCmdDrawIndexed(cmd, mesh->indexCount, 1, 0, 0, 0);
        };

        // Targets first, so a draw sampling one reads what this frame drew
        // into it, in order of first use.
        for (const TargetRun &run : runs)
        {
            if (!run.image || run.copied)
                continue;
            // Transparent black, as the GS reads memory nothing drew.
            VkClearValue clears[2]{};
            clears[1].depthStencil = {0.0f, 0};
            VkRenderPassBeginInfo begin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
            begin.renderPass = s.targetPass;
            begin.framebuffer = run.image->framebuffer;
            begin.renderArea = {{0, 0}, {run.width, run.height}};
            begin.clearValueCount = 2;
            begin.pClearValues = clears;
            vkCmdBeginRenderPass(cmd, &begin, VK_SUBPASS_CONTENTS_INLINE);
            boundCamera = UINT32_MAX;
            for (size_t i : run.draws)
                drawOne(i, static_cast<float>(run.width), static_cast<float>(run.height));
            vkCmdEndRenderPass(cmd);
            s.fillTargetLevels(cmd, *run.image);
        }

        VkClearValue clears[2]{};
        for (int i = 0; i < 4; ++i)
            clears[0].color.float32[i] = frame.clear[i];
        // The clear writes the clear colour's alpha, whose top bit 1.0 sets.
        clears[1].depthStencil = {0.0f, frame.clear[3] >= 1.0f ? 1u : 0u};
        VkRenderPassBeginInfo begin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        begin.renderPass = copyRuns.empty() ? s.renderPass : s.firstPass;
        begin.framebuffer = s.framebuffer;
        begin.renderArea = {{0, 0}, {width, height}};
        begin.clearValueCount = 2;
        begin.pClearValues = clears;
        vkCmdBeginRenderPass(cmd, &begin, VK_SUBPASS_CONTENTS_INLINE);
        boundCamera = UINT32_MAX;
        size_t nextCopy = 0;
        for (size_t i = 0; i < frame.draws.size(); ++i)
        {
            if (drawRun[i] >= 0)
                continue;
            if (nextCopy < copyRuns.size() && runs[copyRuns[nextCopy]].before <= i)
            {
                // The pass ends with the picture so far in the target, is
                // copied from, and goes on from what it kept.
                vkCmdEndRenderPass(cmd);
                for (; nextCopy < copyRuns.size() && runs[copyRuns[nextCopy]].before <= i; ++nextCopy)
                {
                    const TargetRun &run = runs[copyRuns[nextCopy]];
                    if (run.image)
                        s.copyFromTarget(cmd, *run.image, run.x, run.y);
                }
                begin.renderPass = s.resumePass;
                vkCmdBeginRenderPass(cmd, &begin, VK_SUBPASS_CONTENTS_INLINE);
                boundCamera = UINT32_MAX;
            }
            drawOne(i, static_cast<float>(width), static_cast<float>(height));
        }
        vkCmdEndRenderPass(cmd);
        s.retire(serial);
        return true;
    }
}
