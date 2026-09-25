#include "SatEnvProbes.h"
#include "../Log.h"
#include "../VulkanContext.h"

#include <algorithm>
#include <cmath>
#include <random>
#include <stdexcept>
#include <string>

namespace
{
template <typename T> void destroy(VkDevice d, T &h, void (*fn)(VkDevice, T, const VkAllocationCallbacks *))
{
    if (h != VK_NULL_HANDLE)
        fn(d, h, nullptr);
    h = VK_NULL_HANDLE;
}

void allocImage(VulkanContext &ctx, const VkImageCreateInfo &ci, VkImage &img, VkDeviceMemory &mem)
{
    if (vkCreateImage(ctx.device, &ci, nullptr, &img) != VK_SUCCESS)
        throw std::runtime_error("SatEnvProbes: vkCreateImage failed");
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(ctx.device, img, &req);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = ctx.findMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (vkAllocateMemory(ctx.device, &ai, nullptr, &mem) != VK_SUCCESS)
        throw std::runtime_error("SatEnvProbes: vkAllocateMemory failed");
    vkBindImageMemory(ctx.device, img, mem, 0);
}

VkImageView makeView(VkDevice d, VkImage img, VkImageViewType type, uint32_t baseMip, uint32_t mips,
                     uint32_t baseLayer, uint32_t layers)
{
    VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vci.image = img;
    vci.viewType = type;
    vci.format = SatEnvProbes::kFormat;
    vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, baseMip, mips, baseLayer, layers};
    VkImageView v = VK_NULL_HANDLE;
    if (vkCreateImageView(d, &vci, nullptr, &v) != VK_SUCCESS)
        throw std::runtime_error("SatEnvProbes: vkCreateImageView failed");
    return v;
}

void barrier(VkCommandBuffer cmd, VkImage img, VkImageLayout from, VkImageLayout to, VkAccessFlags srcAccess,
             VkAccessFlags dstAccess, VkPipelineStageFlags srcStage, VkPipelineStageFlags dstStage, uint32_t baseMip,
             uint32_t mips, uint32_t layers, uint32_t baseLayer = 0)
{
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.oldLayout = from;
    b.newLayout = to;
    b.srcAccessMask = srcAccess;
    b.dstAccessMask = dstAccess;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, baseMip, mips, baseLayer, layers};
    vkCmdPipelineBarrier(cmd, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &b);
}

VkRenderPass makeColorPass(VkDevice d, VkImageLayout finalLayout)
{
    VkAttachmentDescription att{};
    att.format = SatEnvProbes::kFormat;
    att.samples = VK_SAMPLE_COUNT_1_BIT;
    att.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; // every pixel is drawn
    att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    att.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    att.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    att.finalLayout = finalLayout;
    VkAttachmentReference ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sub{};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1;
    sub.pColorAttachments = &ref;
    // In: earlier reads of the target (last frame's mesh draws / SH projection / UI). Out: this
    // frame's transfers (mip blits) and shader reads.
    VkSubpassDependency dep[2] = {};
    dep[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    dep[0].dstSubpass = 0;
    dep[0].srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
    dep[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep[0].srcAccessMask = 0;
    dep[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    dep[1].srcSubpass = 0;
    dep[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    dep[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep[1].dstStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    dep[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    dep[1].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_SHADER_READ_BIT;
    VkRenderPassCreateInfo rpci{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    rpci.attachmentCount = 1;
    rpci.pAttachments = &att;
    rpci.subpassCount = 1;
    rpci.pSubpasses = &sub;
    rpci.dependencyCount = 2;
    rpci.pDependencies = dep;
    VkRenderPass rp = VK_NULL_HANDLE;
    if (vkCreateRenderPass(d, &rpci, nullptr, &rp) != VK_SUCCESS)
        throw std::runtime_error("SatEnvProbes: render pass");
    return rp;
}
} // namespace

uint32_t SatEnvProbes::mipCount(int slot)
{
    uint32_t m = 1;
    for (uint32_t s = faceSize(slot); s > 1; s >>= 1)
        ++m;
    return m;
}

// ─── Cube faces ───────────────────────────────────────────────────────────────────────────────────
glm::dmat3 SatEnvProbes::faceCamToWorld(int face)
{
    // Columns: where camera +x, +y, +z point. Texel (s, t) of a face (t down) is the camera ray
    // (2s − 1, −(2t − 1), −1), and Vulkan's face selection maps it to these world directions.
    switch (face)
    {
    case 0: return glm::dmat3(glm::dvec3(0, 0, -1), glm::dvec3(0, 1, 0), glm::dvec3(-1, 0, 0)); // +X
    case 1: return glm::dmat3(glm::dvec3(0, 0, 1), glm::dvec3(0, 1, 0), glm::dvec3(1, 0, 0));   // -X
    case 2: return glm::dmat3(glm::dvec3(1, 0, 0), glm::dvec3(0, 0, -1), glm::dvec3(0, -1, 0)); // +Y
    case 3: return glm::dmat3(glm::dvec3(1, 0, 0), glm::dvec3(0, 0, 1), glm::dvec3(0, 1, 0));   // -Y
    case 4: return glm::dmat3(glm::dvec3(1, 0, 0), glm::dvec3(0, 1, 0), glm::dvec3(0, 0, -1));  // +Z
    default: return glm::dmat3(glm::dvec3(-1, 0, 0), glm::dvec3(0, 1, 0), glm::dvec3(0, 0, 1)); // -Z
    }
}

bool SatEnvProbes::faceSelfCheck(double &maxErr)
{
    // Vulkan's cube map face selection (spec, "Cube Map Face Selection and Transformations").
    auto select = [](glm::dvec3 r, int &face, double &s, double &t) {
        const glm::dvec3 a = glm::abs(r);
        double sc, tc, ma;
        if (a.x >= a.y && a.x >= a.z)
        {
            face = r.x > 0 ? 0 : 1;
            sc = r.x > 0 ? -r.z : r.z;
            tc = -r.y;
            ma = a.x;
        }
        else if (a.y >= a.z)
        {
            face = r.y > 0 ? 2 : 3;
            sc = r.x;
            tc = r.y > 0 ? r.z : -r.z;
            ma = a.y;
        }
        else
        {
            face = r.z > 0 ? 4 : 5;
            sc = r.z > 0 ? r.x : -r.x;
            tc = -r.y;
            ma = a.z;
        }
        s = 0.5 * (sc / ma + 1.0);
        t = 0.5 * (tc / ma + 1.0);
    };
    std::mt19937 rng(5);
    std::uniform_real_distribution<double> u(0.02, 0.98);
    maxErr = 0.0;
    bool ok = true;
    for (int f = 0; f < 6; ++f)
        for (int k = 0; k < 64; ++k)
        {
            const double s = u(rng), t = u(rng);
            const glm::dvec3 d = faceCamToWorld(f) * glm::dvec3(2.0 * s - 1.0, -(2.0 * t - 1.0), -1.0);
            int f2;
            double s2, t2;
            select(d, f2, s2, t2);
            ok &= f2 == f;
            maxErr = std::max(maxErr, std::max(std::abs(s2 - s), std::abs(t2 - t)));
        }
    return ok && maxErr < 1e-9;
}

// ─── init / cleanup ───────────────────────────────────────────────────────────────────────────────
void SatEnvProbes::init(VulkanContext &ctx, VkPipelineLayout skyLayout)
{
    device_ = ctx.device;
    skyLayout_ = skyLayout;
    blitFilter = ctx.bestBlitFilter(kFormat);
    {
        double err = 0.0;
        const bool ok = faceSelfCheck(err);
        Log::line(std::string("env probes: cube face table ") + (ok ? "ok" : "WRONG") + " (max texel err " +
                  std::to_string(err) + ")");
    }

    VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sci.magFilter = VK_FILTER_LINEAR;
    sci.minFilter = VK_FILTER_LINEAR;
    sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.maxLod = (float)mipCount(0);
    vkCreateSampler(ctx.device, &sci, nullptr, &linearSampler);

    probePass = makeColorPass(ctx.device, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    bgPass = makeColorPass(ctx.device, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    // Probe images: cube-compatible, 6 layers, full mip chain.
    for (int slot = 0; slot < kProbes; ++slot)
    {
        Probe &p = probes[slot];
        p.size = faceSize(slot);
        p.mips = mipCount(slot);
        const uint32_t kFaceSize = p.size, kMips = p.mips;
        VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ci.flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
        ci.imageType = VK_IMAGE_TYPE_2D;
        ci.format = kFormat;
        ci.extent = {kFaceSize, kFaceSize, 1};
        ci.mipLevels = kMips;
        ci.arrayLayers = 6;
        ci.samples = VK_SAMPLE_COUNT_1_BIT;
        ci.tiling = VK_IMAGE_TILING_OPTIMAL;
        ci.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                   VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        allocImage(ctx, ci, p.image, p.mem);
        p.cubeView = makeView(ctx.device, p.image, VK_IMAGE_VIEW_TYPE_CUBE, 0, kMips, 0, 6);
        for (uint32_t f = 0; f < 6; ++f)
        {
            p.faceView[f] = makeView(ctx.device, p.image, VK_IMAGE_VIEW_TYPE_2D, 0, 1, f, 1);
            VkFramebufferCreateInfo fci{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
            fci.renderPass = probePass;
            fci.attachmentCount = 1;
            fci.pAttachments = &p.faceView[f];
            fci.width = fci.height = kFaceSize;
            fci.layers = 1;
            if (vkCreateFramebuffer(ctx.device, &fci, nullptr, &p.fb[f]) != VK_SUCCESS)
                throw std::runtime_error("SatEnvProbes: framebuffer");
        }
    }

    // SH buffer (device-local), zeroed below.
    ctx.createBuffer(shBufferSize(), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                     VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, shBuf, shMem);

    // Every probe black and readable, the SH zero, before anything samples them.
    {
        VkCommandBuffer cmd = ctx.beginOneTimeCommands();
        for (Probe &p : probes)
        {
            const uint32_t kMips = p.mips;
            barrier(cmd, p.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                    VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                    kMips, 6);
            VkClearColorValue black{};
            VkImageSubresourceRange rng{VK_IMAGE_ASPECT_COLOR_BIT, 0, kMips, 0, 6};
            vkCmdClearColorImage(cmd, p.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &rng);
            barrier(cmd, p.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                    VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, kMips, 6);
        }
        vkCmdFillBuffer(cmd, shBuf, 0, VK_WHOLE_SIZE, 0);
        ctx.endOneTimeCommands(cmd);
    }

    // Descriptor sets: sat_mesh.frag set 1 (the probe's cube), and the SH projection's.
    {
        VkDescriptorSetLayoutBinding b{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT,
                                       nullptr};
        VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        li.bindingCount = 1;
        li.pBindings = &b;
        vkCreateDescriptorSetLayout(ctx.device, &li, nullptr, &meshSetLayout);
        VkDescriptorSetLayoutBinding sb[2] = {
            {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}};
        li.bindingCount = 2;
        li.pBindings = sb;
        vkCreateDescriptorSetLayout(ctx.device, &li, nullptr, &shSetLayout);

        VkDescriptorPoolSize ps[2] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2 * kProbes},
                                      {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, kProbes}};
        VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        pi.poolSizeCount = 2;
        pi.pPoolSizes = ps;
        pi.maxSets = 2 * kProbes;
        vkCreateDescriptorPool(ctx.device, &pi, nullptr, &pool);
        for (int i = 0; i < kProbes; ++i)
        {
            VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
            ai.descriptorPool = pool;
            ai.descriptorSetCount = 1;
            ai.pSetLayouts = &meshSetLayout;
            vkAllocateDescriptorSets(ctx.device, &ai, &meshSets[i]);
            ai.pSetLayouts = &shSetLayout;
            vkAllocateDescriptorSets(ctx.device, &ai, &shSets[i]);
            VkDescriptorImageInfo ii{linearSampler, probes[i].cubeView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
            VkDescriptorBufferInfo bi{shBuf, 0, VK_WHOLE_SIZE};
            VkWriteDescriptorSet w[3] = {};
            w[0] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, meshSets[i], 0, 0, 1,
                    VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &ii, nullptr, nullptr};
            w[1] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, shSets[i], 0, 0, 1,
                    VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &ii, nullptr, nullptr};
            w[2] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, shSets[i], 1, 0, 1,
                    VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &bi, nullptr};
            vkUpdateDescriptorSets(ctx.device, 3, w, 0, nullptr);
        }
    }

    // SH projection pipeline.
    {
        VkPushConstantRange pcr{VK_SHADER_STAGE_COMPUTE_BIT, 0, 8};
        VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pli.setLayoutCount = 1;
        pli.pSetLayouts = &shSetLayout;
        pli.pushConstantRangeCount = 1;
        pli.pPushConstantRanges = &pcr;
        vkCreatePipelineLayout(ctx.device, &pli, nullptr, &shLayout);
        VkShaderModule cs = ctx.loadShader("shaders/env_probe_sh.comp.spv");
        VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        ci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, cs,
                    "main", nullptr};
        ci.layout = shLayout;
        if (vkCreateComputePipelines(ctx.device, VK_NULL_HANDLE, 1, &ci, nullptr, &shPipe) != VK_SUCCESS)
            throw std::runtime_error("SatEnvProbes: SH pipeline");
        vkDestroyShaderModule(ctx.device, cs, nullptr);
    }

    // The env sky pipeline: sat_sky.vert + the SKY_ENV fragment, the sky's own layout and set,
    // dynamic viewport (probe faces and the viewer background differ in size), no depth.
    {
        VkShaderModule vs = ctx.loadShader("shaders/sat_sky.vert.spv");
        VkShaderModule fs = ctx.loadShader("shaders/sat_sky_env.frag.spv");
        VkPipelineShaderStageCreateInfo st[2] = {};
        st[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, vs,
                 "main", nullptr};
        st[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fs,
                 "main", nullptr};
        VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
        ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineViewportStateCreateInfo vps{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
        vps.viewportCount = 1;
        vps.scissorCount = 1;
        VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo dys{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
        dys.dynamicStateCount = 2;
        dys.pDynamicStates = dyn;
        VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
        rs.polygonMode = VK_POLYGON_MODE_FILL;
        rs.cullMode = VK_CULL_MODE_NONE;
        rs.lineWidth = 1.0f;
        VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
        ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
        VkPipelineColorBlendAttachmentState cba{};
        cba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT |
                             VK_COLOR_COMPONENT_A_BIT;
        VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        cb.attachmentCount = 1;
        cb.pAttachments = &cba;
        VkGraphicsPipelineCreateInfo ci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        ci.stageCount = 2;
        ci.pStages = st;
        ci.pVertexInputState = &vi;
        ci.pInputAssemblyState = &ia;
        ci.pViewportState = &vps;
        ci.pRasterizationState = &rs;
        ci.pMultisampleState = &ms;
        ci.pDepthStencilState = &ds;
        ci.pColorBlendState = &cb;
        ci.pDynamicState = &dys;
        ci.layout = skyLayout;
        ci.renderPass = probePass; // bgPass is compatible (same single RGBA16F attachment)
        if (vkCreateGraphicsPipelines(ctx.device, VK_NULL_HANDLE, 1, &ci, nullptr, &envPipe) != VK_SUCCESS)
            throw std::runtime_error("SatEnvProbes: env sky pipeline");
        vkDestroyShaderModule(ctx.device, vs, nullptr);
        vkDestroyShaderModule(ctx.device, fs, nullptr);
    }
    Log::line("env probes: viewer 6 x " + std::to_string(kViewerFaceSize) + "^2, " + std::to_string(kProbes - 1) +
              " scene probes of 6 x " + std::to_string(kSceneFaceSize) + "^2");
}

void SatEnvProbes::destroyViewerBg()
{
    destroy(device_, bgFb, vkDestroyFramebuffer);
    destroy(device_, bgView, vkDestroyImageView);
    destroy(device_, bgImage, vkDestroyImage);
    destroy(device_, bgMem, vkFreeMemory);
    bgW = bgH = 0;
}

void SatEnvProbes::cleanup(VkDevice d)
{
    destroyViewerBg();
    for (Probe &p : probes)
    {
        for (int f = 0; f < 6; ++f)
        {
            destroy(d, p.fb[f], vkDestroyFramebuffer);
            destroy(d, p.faceView[f], vkDestroyImageView);
        }
        destroy(d, p.cubeView, vkDestroyImageView);
        destroy(d, p.image, vkDestroyImage);
        destroy(d, p.mem, vkFreeMemory);
    }
    destroy(d, envPipe, vkDestroyPipeline);
    destroy(d, shPipe, vkDestroyPipeline);
    destroy(d, shLayout, vkDestroyPipelineLayout);
    destroy(d, pool, vkDestroyDescriptorPool);
    destroy(d, meshSetLayout, vkDestroyDescriptorSetLayout);
    destroy(d, shSetLayout, vkDestroyDescriptorSetLayout);
    destroy(d, shBuf, vkDestroyBuffer);
    destroy(d, shMem, vkFreeMemory);
    destroy(d, probePass, vkDestroyRenderPass);
    destroy(d, bgPass, vkDestroyRenderPass);
    destroy(d, linearSampler, vkDestroySampler);
}

// ─── Probe rendering ──────────────────────────────────────────────────────────────────────────────
void SatEnvProbes::recordProbe(VkCommandBuffer cmd, int slot, VkDescriptorSet skyDescSet, const void *pcs,
                               uint32_t pcSize, uint32_t faceMask)
{
    if (!envPipe || slot < 0 || slot >= kProbes || (faceMask & 0x3Fu) == 0u)
        return;
    Probe &p = probes[slot];
    const uint32_t kFaceSize = p.size, kMips = p.mips;
    const VkViewport vp{0.0f, 0.0f, (float)kFaceSize, (float)kFaceSize, 0.0f, 1.0f};
    const VkRect2D sc{{0, 0}, {kFaceSize, kFaceSize}};
    for (int f = 0; f < 6; ++f)
    {
        if ((faceMask & (1u << f)) == 0u)
        {
            // Kept from its last render: mip 0 of this face joins the others as the blit source.
            barrier(cmd, p.image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                    VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                    VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, 1, (uint32_t)f);
            continue;
        }
        VkRenderPassBeginInfo rbi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        rbi.renderPass = probePass;
        rbi.framebuffer = p.fb[f];
        rbi.renderArea = sc;
        vkCmdBeginRenderPass(cmd, &rbi, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdSetViewport(cmd, 0, 1, &vp);
        vkCmdSetScissor(cmd, 0, 1, &sc);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, envPipe);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, skyLayout_, 0, 1, &skyDescSet, 0, nullptr);
        vkCmdPushConstants(cmd, skyLayout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, pcSize,
                           static_cast<const char *>(pcs) + (size_t)f * pcSize);
        vkCmdDraw(cmd, 3, 1, 0, 0);
        vkCmdEndRenderPass(cmd); // mip 0 of face f: TRANSFER_SRC
    }

    // Mip chain (a box pyramid: the reflection of rougher surfaces reads coarser levels).
    for (uint32_t m = 1; m < kMips; ++m)
    {
        barrier(cmd, p.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT, m, 1, 6);
        const int32_t src = (int32_t)(kFaceSize >> (m - 1)), dst = (int32_t)std::max(1u, kFaceSize >> m);
        VkImageBlit blit{};
        blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, m - 1, 0, 6};
        blit.srcOffsets[1] = {src, src, 1};
        blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, m, 0, 6};
        blit.dstOffsets[1] = {dst, dst, 1};
        vkCmdBlitImage(cmd, p.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, p.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                       1, &blit, blitFilter);
        barrier(cmd, p.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT, m, 1, 6);
    }
    barrier(cmd, p.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_SHADER_READ_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0,
            kMips, 6);

    // SH irradiance of the new probe.
    {
        VkBufferMemoryBarrier pre{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
        pre.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
        pre.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        pre.srcQueueFamilyIndex = pre.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        pre.buffer = shBuf;
        pre.size = VK_WHOLE_SIZE;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 1, &pre, 0, nullptr);
        struct
        {
            uint32_t slot;
            float lod;
        } pc{(uint32_t)slot, (float)(kMips - 5)}; // the 16² level
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, shPipe);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, shLayout, 0, 1, &shSets[slot], 0, nullptr);
        vkCmdPushConstants(cmd, shLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
        vkCmdDispatch(cmd, 1, 1, 1);
        VkBufferMemoryBarrier post = pre;
        post.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        post.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr,
                             1, &post, 0, nullptr);
    }
}

// ─── Viewer background ────────────────────────────────────────────────────────────────────────────
bool SatEnvProbes::ensureViewerBg(VulkanContext &ctx, uint32_t w, uint32_t h)
{
    w = std::max(w, 16u);
    h = std::max(h, 16u);
    if (w == bgW && h == bgH && bgImage)
        return false;
    vkDeviceWaitIdle(ctx.device);
    destroyViewerBg();
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = kFormat;
    ci.extent = {w, h, 1};
    ci.mipLevels = 1;
    ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    allocImage(ctx, ci, bgImage, bgMem);
    bgView = makeView(ctx.device, bgImage, VK_IMAGE_VIEW_TYPE_2D, 0, 1, 0, 1);
    VkFramebufferCreateInfo fci{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    fci.renderPass = bgPass;
    fci.attachmentCount = 1;
    fci.pAttachments = &bgView;
    fci.width = w;
    fci.height = h;
    fci.layers = 1;
    if (vkCreateFramebuffer(ctx.device, &fci, nullptr, &bgFb) != VK_SUCCESS)
        throw std::runtime_error("SatEnvProbes: viewer background framebuffer");
    bgW = w;
    bgH = h;
    VkCommandBuffer cmd = ctx.beginOneTimeCommands();
    barrier(cmd, bgImage, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0,
            VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 1, 1);
    ctx.endOneTimeCommands(cmd);
    return true;
}

void SatEnvProbes::recordViewerBg(VkCommandBuffer cmd, VkDescriptorSet skyDescSet, const void *pc, uint32_t pcSize)
{
    if (!envPipe || !bgFb)
        return;
    const VkViewport vp{0.0f, 0.0f, (float)bgW, (float)bgH, 0.0f, 1.0f};
    const VkRect2D sc{{0, 0}, {bgW, bgH}};
    VkRenderPassBeginInfo rbi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rbi.renderPass = bgPass;
    rbi.framebuffer = bgFb;
    rbi.renderArea = sc;
    vkCmdBeginRenderPass(cmd, &rbi, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdSetViewport(cmd, 0, 1, &vp);
    vkCmdSetScissor(cmd, 0, 1, &sc);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, envPipe);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, skyLayout_, 0, 1, &skyDescSet, 0, nullptr);
    vkCmdPushConstants(cmd, skyLayout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, pcSize, pc);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRenderPass(cmd);
}
