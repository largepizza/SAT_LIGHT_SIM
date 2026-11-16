// Plume bricks (2026-11-15, .plans/PLUME_OVERHAUL_PLAN.md phase 2; issue_20261115_165031 "Staging billow cloud").
// The GPU side: the brick buffer (launchFillVisuals fills it), the 64^3-per-brick extinction atlas, the half-res
// outputs cloud_march.comp reads (bindings 27 / 28), and the three passes of shaders/plume_brick.comp (splat, the
// Sun's optical depth per voxel, march).
// Recorded by recordCompute right before the composite. docs/subsystems/launches.md "Plume bricks".

#include "SatelliteSim.h"
#include "../VulkanContext.h"
#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace
{
VkImageView brickView(VkDevice dev, VkImage img, VkImageViewType type, VkFormat fmt)
{
    VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vci.image = img;
    vci.viewType = type;
    vci.format = fmt;
    vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkImageView v = VK_NULL_HANDLE;
    if (vkCreateImageView(dev, &vci, nullptr, &v) != VK_SUCCESS)
        throw std::runtime_error("plume bricks: vkCreateImageView failed");
    return v;
}

void computeBarrier(VkCommandBuffer cmd, VkAccessFlags src, VkAccessFlags dst)
{
    VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    mb.srcAccessMask = src;
    mb.dstAccessMask = dst;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &mb, 0,
                         nullptr, 0, nullptr);
}
} // namespace

void SatelliteSim::createPlumeBricks(VulkanContext &ctx)
{
    const VkDevice dev = ctx.device;
    // The brick list: header (count), the bricks, their Gaussians (GpuDustCloud, eye-relative ENU like the dust list).
    brickMapped = createMirrored(ctx, 16 + sizeof(GpuPlumeBrick) * kMaxPlumeBricks + sizeof(GpuDustCloud) * kMaxBrickGaussians,
                                 VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, brickBuf, brickMem, true, "plumeBricks");
    // The atlas: kBrickRes^2 x (kBrickRes x kMaxPlumeBricks), RGBA16F (rgb extinction x light colour, a extinction:
    // each region keeps its own sunlight colour), kept in GENERAL (written by the splat, sampled by the march).
    ctx.createImage(kBrickRes, kBrickRes, VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                    brickVolImg, brickVolMem, 1, kBrickRes * kMaxPlumeBricks);
    brickVolView = brickView(dev, brickVolImg, VK_IMAGE_VIEW_TYPE_3D, VK_FORMAT_R16G16B16A16_SFLOAT);
    // The shadow atlas (same layout): per voxel the Sun's optical depth through the brick, lobes and all (mode 2), so the
    // march reads its light in one fetch a sample instead of a light march a sample.
    ctx.createImage(kBrickRes, kBrickRes, VK_FORMAT_R16_SFLOAT, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                    brickShadowImg, brickShadowMem, 1, kBrickRes * kMaxPlumeBricks);
    brickShadowView = brickView(dev, brickShadowImg, VK_IMAGE_VIEW_TYPE_3D, VK_FORMAT_R16_SFLOAT);
    {
        VkCommandBuffer cmd = ctx.beginOneTimeCommands();
        for (VkImage img : {brickVolImg, brickShadowImg})
            ctx.imageBarrier(cmd, img, 0, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                             VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
        ctx.endOneTimeCommands(cmd);
    }

    const VkDescriptorType kTypes[10] = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,         VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                                         VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                         VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,          VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                                         VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                                         VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER};
    VkDescriptorSetLayoutBinding b[10];
    for (uint32_t i = 0; i < 10; ++i)
        b[i] = {i, kTypes[i], 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    li.bindingCount = 10;
    li.pBindings = b;
    vkCreateDescriptorSetLayout(dev, &li, nullptr, &brickDescLayout);
    VkDescriptorPoolSize ps[3] = {{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1},
                                  {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 4},
                                  {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 5}};
    VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pi.maxSets = 1;
    pi.poolSizeCount = 3;
    pi.pPoolSizes = ps;
    vkCreateDescriptorPool(dev, &pi, nullptr, &brickDescPool);
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = brickDescPool;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &brickDescLayout;
    vkAllocateDescriptorSets(dev, &ai, &brickDescSet);

    VkPushConstantRange pcr{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(CloudMarchPC)};
    VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &brickDescLayout;
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges = &pcr;
    vkCreatePipelineLayout(dev, &pli, nullptr, &brickPipeLayout);
    if (!buildPlumeBrickPipelines(ctx, "").empty())
        throw std::runtime_error("plume bricks: pipeline");
    createPlumeBrickTargets(ctx);
}

// The splat, march and shadow pipelines (specialization constant 0: modes 0, 1, 2) from an SPV: the shipped one ("") or
// a harness variant (`shaders reload brick=<spv>`). Replaces the current ones only when all build; returns an error or "".
std::string SatelliteSim::buildPlumeBrickPipelines(VulkanContext &ctx, const std::string &spv)
{
    VkDevice dev = ctx.device;
    VkShaderModule mod = VK_NULL_HANDLE;
    try
    {
        mod = ctx.loadShader(spv.empty() ? std::string("shaders/plume_brick.comp.spv") : spv);
    }
    catch (const std::exception &ex)
    {
        return ex.what();
    }
    VkPipeline np[3] = {VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE};
    std::string err;
    for (int mode = 0; mode < 3; ++mode)
    {
        const int32_t m = mode;
        VkSpecializationMapEntry e{0, 0, 4};
        VkSpecializationInfo si{1, &e, 4, &m};
        VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        ci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, mod, "main", &si};
        ci.layout = brickPipeLayout;
        if (vkCreateComputePipelines(dev, ctx.pipelineCache, 1, &ci, nullptr, &np[mode]) != VK_SUCCESS)
            err = "pipeline creation failed";
    }
    vkDestroyShaderModule(dev, mod, nullptr);
    if (!err.empty())
    {
        for (VkPipeline p : np)
            if (p != VK_NULL_HANDLE)
                vkDestroyPipeline(dev, p, nullptr);
        return err;
    }
    if (brickSplatPipe != VK_NULL_HANDLE)
        vkDestroyPipeline(dev, brickSplatPipe, nullptr);
    if (brickMarchPipe != VK_NULL_HANDLE)
        vkDestroyPipeline(dev, brickMarchPipe, nullptr);
    if (brickShadowPipe != VK_NULL_HANDLE)
        vkDestroyPipeline(dev, brickShadowPipe, nullptr);
    brickSplatPipe = np[0];
    brickMarchPipe = np[1];
    brickShadowPipe = np[2];
    return "";
}

void SatelliteSim::createPlumeBrickTargets(VulkanContext &ctx)
{
    const VkExtent2D e = computeHalfExtent(ctx);   // cloud_march.comp's dispatch grid
    brickExt = e;
    ctx.createImage(e.width, e.height, VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                    brickLImg, brickLMem);
    ctx.createImage(e.width, e.height, VK_FORMAT_R32G32B32A32_SFLOAT, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                    brickTImg, brickTMem);
    brickLView = brickView(ctx.device, brickLImg, VK_IMAGE_VIEW_TYPE_2D, VK_FORMAT_R16G16B16A16_SFLOAT);
    brickTView = brickView(ctx.device, brickTImg, VK_IMAGE_VIEW_TYPE_2D, VK_FORMAT_R32G32B32A32_SFLOAT);
    VkCommandBuffer cmd = ctx.beginOneTimeCommands();
    for (VkImage img : {brickLImg, brickTImg})
        ctx.imageBarrier(cmd, img, 0, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                         VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    ctx.endOneTimeCommands(cmd);
    writePlumeBrickDescriptors(ctx);
}

void SatelliteSim::destroyPlumeBrickTargets(VkDevice dev)
{
    for (VkImageView *v : {&brickLView, &brickTView})
        if (*v)
            vkDestroyImageView(dev, *v, nullptr), *v = VK_NULL_HANDLE;
    for (VkImage *i : {&brickLImg, &brickTImg})
        if (*i)
            vkDestroyImage(dev, *i, nullptr), *i = VK_NULL_HANDLE;
    for (VkDeviceMemory *m : {&brickLMem, &brickTMem})
        if (*m)
            vkFreeMemory(dev, *m, nullptr), *m = VK_NULL_HANDLE;
}

void SatelliteSim::writePlumeBrickDescriptors(VulkanContext &ctx)
{
    if (!brickDescSet || !brickLView)
        return;
    VkDescriptorBufferInfo bufI{brickBuf, 0, VK_WHOLE_SIZE};
    VkDescriptorImageInfo volW{VK_NULL_HANDLE, brickVolView, VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorImageInfo volR{cloudMarchSampler, brickVolView, VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorImageInfo lobe{cv2RepeatSampler, cv2ShapeBillowView ? cv2ShapeBillowView : cv2ShapeView,
                               VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkDescriptorImageInfo outL{VK_NULL_HANDLE, brickLView, VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorImageInfo outT{VK_NULL_HANDLE, brickTView, VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorImageInfo depth{sceneDepthSampler, sceneDepthView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkDescriptorImageInfo cmL{cloudMarchSampler, brickLView, VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorImageInfo cmT{cloudMarchSampler, brickTView, VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorImageInfo shW{VK_NULL_HANDLE, brickShadowView, VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorImageInfo shR{cloudMarchSampler, brickShadowView, VK_IMAGE_LAYOUT_GENERAL};
    // the exhaust ice's Mie table (clamped, filtered: angle x radius row); the 1x1 stand-in when missing
    VkDescriptorImageInfo mie{cv2ClampSampler, plumeMieView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    auto W = [](VkDescriptorSet s, uint32_t bind, VkDescriptorType t, const VkDescriptorImageInfo *ii,
                const VkDescriptorBufferInfo *bi) {
        return VkWriteDescriptorSet{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, s, bind, 0, 1, t, ii, bi, nullptr};
    };
    VkWriteDescriptorSet w[13] = {
        W(brickDescSet, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &bufI),
        W(brickDescSet, 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &volW, nullptr),
        W(brickDescSet, 2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &volR, nullptr),
        W(brickDescSet, 3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &lobe, nullptr),
        W(brickDescSet, 4, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &outL, nullptr),
        W(brickDescSet, 5, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &outT, nullptr),
        W(brickDescSet, 6, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &depth, nullptr),
        W(brickDescSet, 7, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &shW, nullptr),
        W(brickDescSet, 8, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &shR, nullptr),
        W(brickDescSet, 9, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &mie, nullptr),
        W(cloudMarchDescSet, 29, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &mie, nullptr),
        W(cloudMarchDescSet, 27, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &cmL, nullptr),
        W(cloudMarchDescSet, 28, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &cmT, nullptr)};
    frameSafeUpdateDescriptorSets(ctx.device, 13, w, 0, nullptr);
}

void SatelliteSim::recordPlumeBricks(VkCommandBuffer cmd, VulkanContext &ctx, const CloudMarchPC &cpc)
{
    if (bricks_.empty() || !brickMarchPipe || !brickLView)
        return;
    (void)ctx;
    // the previous frame's reads of the atlas / outputs before this frame's writes
    computeBarrier(cmd, VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_SHADER_WRITE_BIT);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, brickPipeLayout, 0, 1, &brickDescSet, 0, nullptr);
    vkCmdPushConstants(cmd, brickPipeLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(cpc), &cpc);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, brickSplatPipe);
    vkCmdDispatch(cmd, kBrickRes / 8, kBrickRes / 8, kBrickRes * (uint32_t)bricks_.size());
    computeBarrier(cmd, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
    // the Sun's optical depth per voxel (reads the splat)
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, brickShadowPipe);
    vkCmdDispatch(cmd, kBrickRes / 8, kBrickRes / 8, kBrickRes * (uint32_t)bricks_.size());
    computeBarrier(cmd, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, brickMarchPipe);
    vkCmdDispatch(cmd, (brickExt.width + 7) / 8, (brickExt.height + 7) / 8, 1);
    computeBarrier(cmd, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
}

void SatelliteSim::destroyPlumeBricks(VkDevice dev)
{
    destroyPlumeBrickTargets(dev);
    if (brickVolView)
        vkDestroyImageView(dev, brickVolView, nullptr);
    if (brickVolImg)
        vkDestroyImage(dev, brickVolImg, nullptr);
    if (brickVolMem)
        vkFreeMemory(dev, brickVolMem, nullptr);
    brickVolView = VK_NULL_HANDLE;
    brickVolImg = VK_NULL_HANDLE;
    brickVolMem = VK_NULL_HANDLE;
    if (brickShadowView)
        vkDestroyImageView(dev, brickShadowView, nullptr);
    if (brickShadowImg)
        vkDestroyImage(dev, brickShadowImg, nullptr);
    if (brickShadowMem)
        vkFreeMemory(dev, brickShadowMem, nullptr);
    brickShadowView = VK_NULL_HANDLE;
    brickShadowImg = VK_NULL_HANDLE;
    brickShadowMem = VK_NULL_HANDLE;
    if (brickShadowPipe)
        vkDestroyPipeline(dev, brickShadowPipe, nullptr);
    brickShadowPipe = VK_NULL_HANDLE;
    if (brickSplatPipe)
        vkDestroyPipeline(dev, brickSplatPipe, nullptr);
    if (brickMarchPipe)
        vkDestroyPipeline(dev, brickMarchPipe, nullptr);
    if (brickPipeLayout)
        vkDestroyPipelineLayout(dev, brickPipeLayout, nullptr);
    if (brickDescPool)
        vkDestroyDescriptorPool(dev, brickDescPool, nullptr);
    if (brickDescLayout)
        vkDestroyDescriptorSetLayout(dev, brickDescLayout, nullptr);
    brickSplatPipe = brickMarchPipe = VK_NULL_HANDLE;
    brickPipeLayout = VK_NULL_HANDLE;
    brickDescPool = VK_NULL_HANDLE;
    brickDescLayout = VK_NULL_HANDLE;
    brickDescSet = VK_NULL_HANDLE;
    if (brickBuf != VK_NULL_HANDLE)   // (at shutdown, as the dust list's: the mirror is torn down with the context)
    {
        vkDestroyBuffer(dev, brickBuf, nullptr);
        vkFreeMemory(dev, brickMem, nullptr);
        brickBuf = VK_NULL_HANDLE;
        brickMem = VK_NULL_HANDLE;
        brickMapped = nullptr;
    }
}
