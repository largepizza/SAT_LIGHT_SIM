// SatelliteSimCloudsV2.cpp — clouds v2 (.plans/CLOUDS_V2_PLAN.md, phases 1 + 2).
//
// Owns everything v2 adds: the init-time bakes (three 3D noise volumes and the weather cube built
// from the real-Earth map), the CloudV2Params UBO, and the two per-frame passes that run just before
// cloud_march.comp:
//   cloud_v2_march.comp    one pixel of each 2x2 half-res block per frame (quarter the rays)
//   cloud_v2_resolve.comp  every half-res pixel: new sample over reprojected history, or history
// then a copy of the result into the history image for the next frame.
//
// cloud_march.comp composites the resolved result in place of its own cloudMarchCS whenever
// GpuCloudParams::cloudsV2 is set (bindings 15-20 of its set, written by
// writeCloudsV2ConsumerDescriptors); beam_self_march.comp occludes beams with the same field
// (its bindings 5-8). v1 is untouched and remains the default until phase 7.
//
// All screen-sized v2 images live permanently in VK_IMAGE_LAYOUT_GENERAL: they are written as
// storage images, read as sampled images and copied, and GENERAL serves all three, so the passes
// need memory barriers only — no layout bookkeeping to get out of step.

#include "SatelliteSim.h"

#include "Log.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>

namespace
{
constexpr uint32_t kCv2NoiseRes = 128;
constexpr uint32_t kCv2NoiseMips = 8;     // 128 .. 1: the field reads them at the pixel footprint
constexpr uint32_t kCv2WeatherFace = 1024;
constexpr uint32_t kCv2WeatherMips = 8;   // 1024 .. 8

VkImageView makeView(VkDevice dev, VkImage img, VkImageViewType type, VkFormat fmt,
                     uint32_t baseMip, uint32_t mips, uint32_t layers)
{
    VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vci.image = img;
    vci.viewType = type;
    vci.format = fmt;
    vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, baseMip, mips, 0, layers};
    VkImageView v = VK_NULL_HANDLE;
    if (vkCreateImageView(dev, &vci, nullptr, &v) != VK_SUCCESS)
        throw std::runtime_error("clouds v2: vkCreateImageView failed");
    return v;
}

// Whole-image layout transition (all mips/layers) — ctx.imageBarrier only does mip 0 / layer 0.
void transitionAll(VkCommandBuffer cmd, VkImage img, uint32_t mips, uint32_t layers,
                   VkImageLayout from, VkImageLayout to,
                   VkAccessFlags srcA, VkAccessFlags dstA,
                   VkPipelineStageFlags srcS, VkPipelineStageFlags dstS)
{
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask = srcA;
    b.dstAccessMask = dstA;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, mips, 0, layers};
    vkCmdPipelineBarrier(cmd, srcS, dstS, 0, 0, nullptr, 0, nullptr, 1, &b);
}

void memoryBarrier(VkCommandBuffer cmd, VkAccessFlags srcA, VkAccessFlags dstA,
                   VkPipelineStageFlags srcS, VkPipelineStageFlags dstS)
{
    VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    mb.srcAccessMask = srcA;
    mb.dstAccessMask = dstA;
    vkCmdPipelineBarrier(cmd, srcS, dstS, 0, 1, &mb, 0, nullptr, 0, nullptr);
}

VkPipeline makeComputePipeline(VulkanContext &ctx, const char *spv, VkPipelineLayout layout)
{
    VkShaderModule mod = ctx.loadShader(spv);
    VkPipelineShaderStageCreateInfo stage{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    stage.module = mod;
    stage.pName = "main";
    VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    ci.stage = stage;
    ci.layout = layout;
    VkPipeline p = VK_NULL_HANDLE;
    if (vkCreateComputePipelines(ctx.device, VK_NULL_HANDLE, 1, &ci, nullptr, &p) != VK_SUCCESS)
        throw std::runtime_error(std::string("clouds v2: failed to create pipeline ") + spv);
    vkDestroyShaderModule(ctx.device, mod, nullptr);
    return p;
}

VkDescriptorSetLayout makeSetLayout(VkDevice dev, const std::vector<VkDescriptorType> &types)
{
    std::vector<VkDescriptorSetLayoutBinding> b(types.size());
    for (size_t i = 0; i < types.size(); ++i)
        b[i] = {(uint32_t)i, types[i], 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    li.bindingCount = (uint32_t)b.size();
    li.pBindings = b.data();
    VkDescriptorSetLayout l = VK_NULL_HANDLE;
    vkCreateDescriptorSetLayout(dev, &li, nullptr, &l);
    return l;
}

VkWriteDescriptorSet imageWrite(VkDescriptorSet set, uint32_t binding, VkDescriptorType type,
                                const VkDescriptorImageInfo *info)
{
    return {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, set, binding, 0, 1, type, info, nullptr, nullptr};
}
VkWriteDescriptorSet bufferWrite(VkDescriptorSet set, uint32_t binding, VkDescriptorType type,
                                 const VkDescriptorBufferInfo *info)
{
    return {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, set, binding, 0, 1, type, nullptr, info, nullptr};
}

double fracPos(double x) { return x - std::floor(x); }

// Box-filtered 3D mip chain by blits. Mip 0 in GENERAL on entry (just baked); every mip in
// SHADER_READ_ONLY_OPTIMAL on return.
void genMips3D(VkCommandBuffer cmd, VkImage img, uint32_t size, uint32_t mips)
{
    auto barrier = [&](uint32_t mip, VkImageLayout from, VkImageLayout to, VkAccessFlags sa, VkAccessFlags da,
                       VkPipelineStageFlags ss, VkPipelineStageFlags ds)
    {
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        b.srcAccessMask = sa;
        b.dstAccessMask = da;
        b.oldLayout = from;
        b.newLayout = to;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = img;
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, mip, 1, 0, 1};
        vkCmdPipelineBarrier(cmd, ss, ds, 0, 0, nullptr, 0, nullptr, 1, &b);
    };
    barrier(0, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_SHADER_WRITE_BIT,
            VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    for (uint32_t m = 1; m < mips; ++m)
    {
        int32_t s0 = (int32_t)std::max(1u, size >> (m - 1)), s1 = (int32_t)std::max(1u, size >> m);
        barrier(m, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkImageBlit blit{};
        blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, m - 1, 0, 1};
        blit.srcOffsets[1] = {s0, s0, s0};
        blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, m, 0, 1};
        blit.dstOffsets[1] = {s1, s1, s1};
        vkCmdBlitImage(cmd, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                       1, &blit, VK_FILTER_LINEAR);
        barrier(m, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    }
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, mips, 0, 1};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &b);
}
} // namespace

// ─── createCloudsV2 ───────────────────────────────────────────────────────────────────────────────
// After createGlowResources (needs the Earth textures, beamCloudLightBuf, terrainFrameBuf and the
// scene depth image). Bakes run once here with one-shot command buffers.
void SatelliteSim::createCloudsV2(VulkanContext &ctx)
{
    VkDevice dev = ctx.device;

    // ── Samplers ──
    {
        VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        sci.magFilter = VK_FILTER_LINEAR;
        sci.minFilter = VK_FILTER_LINEAR;
        sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        sci.maxLod = (float)kCv2WeatherMips;
        vkCreateSampler(dev, &sci, nullptr, &cv2RepeatSampler);
        sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        sci.maxLod = 0.0f;
        vkCreateSampler(dev, &sci, nullptr, &cv2ClampSampler);
    }

    // ── Noise bakes (cloud_v2_noise.comp, modes 0/1/2) ──
    {
        VkImage *imgs[3] = {&cv2ShapeImg, &cv2DetailImg, &cv2MesoImg};
        VkDeviceMemory *mems[3] = {&cv2ShapeMem, &cv2DetailMem, &cv2MesoMem};
        VkImageView *views[3] = {&cv2ShapeView, &cv2DetailView, &cv2MesoView};
        for (int i = 0; i < 3; ++i)
        {
            ctx.createImage(kCv2NoiseRes, kCv2NoiseRes, VK_FORMAT_R8G8B8A8_UNORM,
                            VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                                VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                            *imgs[i], *mems[i], kCv2NoiseMips, kCv2NoiseRes);
            *views[i] = makeView(dev, *imgs[i], VK_IMAGE_VIEW_TYPE_3D, VK_FORMAT_R8G8B8A8_UNORM, 0, kCv2NoiseMips, 1);
        }

        VkDescriptorSetLayout bl = makeSetLayout(dev, {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE});
        VkPushConstantRange pcr{VK_SHADER_STAGE_COMPUTE_BIT, 0, 8};
        VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pli.setLayoutCount = 1;
        pli.pSetLayouts = &bl;
        pli.pushConstantRangeCount = 1;
        pli.pPushConstantRanges = &pcr;
        VkPipelineLayout pl = VK_NULL_HANDLE;
        vkCreatePipelineLayout(dev, &pli, nullptr, &pl);
        VkPipeline pipe = makeComputePipeline(ctx, "shaders/cloud_v2_noise.comp.spv", pl);

        VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 3};
        VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        pi.maxSets = 3;
        pi.poolSizeCount = 1;
        pi.pPoolSizes = &ps;
        VkDescriptorPool pool = VK_NULL_HANDLE;
        vkCreateDescriptorPool(dev, &pi, nullptr, &pool);
        VkDescriptorSet sets[3];
        VkDescriptorSetLayout lays[3] = {bl, bl, bl};
        VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        ai.descriptorPool = pool;
        ai.descriptorSetCount = 3;
        ai.pSetLayouts = lays;
        vkAllocateDescriptorSets(dev, &ai, sets);

        VkImageView bakeViews[3] = {};   // mip 0 only: a storage view must name one level
        VkCommandBuffer cmd = ctx.beginOneTimeCommands();
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
        for (int i = 0; i < 3; ++i)
        {
            ctx.imageBarrier(cmd, *imgs[i], 0, VK_ACCESS_SHADER_WRITE_BIT,
                             VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                             VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
            bakeViews[i] = makeView(dev, *imgs[i], VK_IMAGE_VIEW_TYPE_3D, VK_FORMAT_R8G8B8A8_UNORM, 0, 1, 1);
            VkDescriptorImageInfo ii{VK_NULL_HANDLE, bakeViews[i], VK_IMAGE_LAYOUT_GENERAL};
            VkWriteDescriptorSet w = imageWrite(sets[i], 0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &ii);
            vkUpdateDescriptorSets(dev, 1, &w, 0, nullptr);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 1, &sets[i], 0, nullptr);
            int32_t pcv[2] = {i, 17 + i * 31};
            vkCmdPushConstants(cmd, pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, 8, pcv);
            vkCmdDispatch(cmd, kCv2NoiseRes / 8, kCv2NoiseRes / 8, kCv2NoiseRes / 8);
            genMips3D(cmd, *imgs[i], kCv2NoiseRes, kCv2NoiseMips);
        }
        ctx.endOneTimeCommands(cmd);
        for (VkImageView v : bakeViews)
            vkDestroyImageView(dev, v, nullptr);
        vkDestroyPipeline(dev, pipe, nullptr);
        vkDestroyPipelineLayout(dev, pl, nullptr);
        vkDestroyDescriptorPool(dev, pool, nullptr);
        vkDestroyDescriptorSetLayout(dev, bl, nullptr);
    }

    // ── Weather cube (cloud_v2_weather.comp, one dispatch per mip) ──
    {
        VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ci.flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
        ci.imageType = VK_IMAGE_TYPE_2D;
        ci.format = VK_FORMAT_R8G8B8A8_UNORM;
        ci.extent = {kCv2WeatherFace, kCv2WeatherFace, 1};
        ci.mipLevels = kCv2WeatherMips;
        ci.arrayLayers = 6;
        ci.samples = VK_SAMPLE_COUNT_1_BIT;
        ci.tiling = VK_IMAGE_TILING_OPTIMAL;
        ci.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (vkCreateImage(dev, &ci, nullptr, &cv2WeatherImg) != VK_SUCCESS)
            throw std::runtime_error("clouds v2: weather cube vkCreateImage failed");
        VkMemoryRequirements req;
        vkGetImageMemoryRequirements(dev, cv2WeatherImg, &req);
        VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        mai.allocationSize = req.size;
        mai.memoryTypeIndex = ctx.findMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        vkAllocateMemory(dev, &mai, nullptr, &cv2WeatherMem);
        vkBindImageMemory(dev, cv2WeatherImg, cv2WeatherMem, 0);
        cv2WeatherView = makeView(dev, cv2WeatherImg, VK_IMAGE_VIEW_TYPE_CUBE, VK_FORMAT_R8G8B8A8_UNORM,
                                  0, kCv2WeatherMips, 6);

        VkDescriptorSetLayout bl = makeSetLayout(dev, {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                                       VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                                       VK_DESCRIPTOR_TYPE_STORAGE_IMAGE});
        VkPushConstantRange pcr{VK_SHADER_STAGE_COMPUTE_BIT, 0, 8};
        VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pli.setLayoutCount = 1;
        pli.pSetLayouts = &bl;
        pli.pushConstantRangeCount = 1;
        pli.pPushConstantRanges = &pcr;
        VkPipelineLayout pl = VK_NULL_HANDLE;
        vkCreatePipelineLayout(dev, &pli, nullptr, &pl);
        VkPipeline pipe = makeComputePipeline(ctx, "shaders/cloud_v2_weather.comp.spv", pl);

        VkDescriptorPoolSize ps[2] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2 * kCv2WeatherMips},
                                      {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, kCv2WeatherMips}};
        VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        pi.maxSets = kCv2WeatherMips;
        pi.poolSizeCount = 2;
        pi.pPoolSizes = ps;
        VkDescriptorPool pool = VK_NULL_HANDLE;
        vkCreateDescriptorPool(dev, &pi, nullptr, &pool);

        // A missing map falls back to the always-valid noise texture, as the other sets do.
        VkDescriptorImageInfo cloudsInfo{earthCloudsSampler ? earthCloudsSampler : noiseSampler,
                                         earthCloudsView ? earthCloudsView : noiseTexView,
                                         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkDescriptorImageInfo specInfo{earthSpecSampler ? earthSpecSampler : noiseSampler,
                                       earthSpecView ? earthSpecView : noiseTexView,
                                       VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};

        std::vector<VkImageView> mipViews(kCv2WeatherMips);
        VkCommandBuffer cmd = ctx.beginOneTimeCommands();
        transitionAll(cmd, cv2WeatherImg, kCv2WeatherMips, 6, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                      0, VK_ACCESS_SHADER_WRITE_BIT,
                      VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
        // The source map is 8192 wide: ~0.044 deg/texel. A 1024 face texel is ~0.088 deg at its
        // centre, one source mip up; each weather mip is one more.
        const float srcLod0 = 1.0f;
        for (uint32_t m = 0; m < kCv2WeatherMips; ++m)
        {
            mipViews[m] = makeView(dev, cv2WeatherImg, VK_IMAGE_VIEW_TYPE_2D_ARRAY, VK_FORMAT_R8G8B8A8_UNORM, m, 1, 6);
            VkDescriptorSet set;
            VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
            ai.descriptorPool = pool;
            ai.descriptorSetCount = 1;
            ai.pSetLayouts = &bl;
            vkAllocateDescriptorSets(dev, &ai, &set);
            VkDescriptorImageInfo outInfo{VK_NULL_HANDLE, mipViews[m], VK_IMAGE_LAYOUT_GENERAL};
            VkWriteDescriptorSet w[3] = {
                imageWrite(set, 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &cloudsInfo),
                imageWrite(set, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &specInfo),
                imageWrite(set, 2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &outInfo)};
            vkUpdateDescriptorSets(dev, 3, w, 0, nullptr);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 1, &set, 0, nullptr);
            struct
            {
                int32_t faceSize;
                float srcLod;
            } pcv{(int32_t)(kCv2WeatherFace >> m), srcLod0 + (float)m};
            vkCmdPushConstants(cmd, pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, 8, &pcv);
            uint32_t g = std::max(1u, ((kCv2WeatherFace >> m) + 7) / 8);
            vkCmdDispatch(cmd, g, g, 6);
        }
        transitionAll(cmd, cv2WeatherImg, kCv2WeatherMips, 6, VK_IMAGE_LAYOUT_GENERAL,
                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                      VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                      VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
        ctx.endOneTimeCommands(cmd);
        for (VkImageView v : mipViews)
            vkDestroyImageView(dev, v, nullptr);
        vkDestroyPipeline(dev, pipe, nullptr);
        vkDestroyPipelineLayout(dev, pl, nullptr);
        vkDestroyDescriptorPool(dev, pool, nullptr);
        vkDestroyDescriptorSetLayout(dev, bl, nullptr);
    }

    // ── UBO ──
    ctx.createBuffer(sizeof(GpuCloudV2Params), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                     cv2ParamsBuf, cv2ParamsMem);
    vkMapMemory(dev, cv2ParamsMem, 0, sizeof(GpuCloudV2Params), 0, &cv2ParamsMapped);
    std::memset(cv2ParamsMapped, 0, sizeof(GpuCloudV2Params));

    // ── Pass layouts, sets, pipelines ──
    using T = VkDescriptorType;
    const T UBO = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, TEX = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            SSBO = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, IMG = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    // cloud_v2_march.comp: 0 CloudParams, 1 CloudV2Params, 2 weather, 3 shape, 4 detail, 5 meso,
    // 6 sceneDepth, 7 earthNight, 8 terrainFrame, 9 beamCloudLights, 10 outColor, 11 outDepth
    cv2MarchDescLayout = makeSetLayout(dev, {UBO, UBO, TEX, TEX, TEX, TEX, TEX, TEX, SSBO, SSBO, IMG, IMG});
    // cloud_v2_resolve.comp: 0 CloudV2Params, 1 new color, 2 new depth, 3 history, 4 out color, 5 out depth
    cv2ResolveDescLayout = makeSetLayout(dev, {UBO, TEX, TEX, TEX, IMG, IMG});
    {
        VkDescriptorPoolSize ps[4] = {{UBO, 3}, {TEX, 9}, {SSBO, 2}, {IMG, 4}};
        VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        pi.maxSets = 2;
        pi.poolSizeCount = 4;
        pi.pPoolSizes = ps;
        vkCreateDescriptorPool(dev, &pi, nullptr, &cv2DescPool);
        VkDescriptorSetLayout lays[2] = {cv2MarchDescLayout, cv2ResolveDescLayout};
        VkDescriptorSet sets[2];
        VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        ai.descriptorPool = cv2DescPool;
        ai.descriptorSetCount = 2;
        ai.pSetLayouts = lays;
        vkAllocateDescriptorSets(dev, &ai, sets);
        cv2MarchDescSet = sets[0];
        cv2ResolveDescSet = sets[1];
    }
    VkPushConstantRange pcr{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(CloudMarchPC)};
    for (int i = 0; i < 2; ++i)
    {
        VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pli.setLayoutCount = 1;
        pli.pSetLayouts = i == 0 ? &cv2MarchDescLayout : &cv2ResolveDescLayout;
        pli.pushConstantRangeCount = 1;
        pli.pPushConstantRanges = &pcr;
        vkCreatePipelineLayout(dev, &pli, nullptr, i == 0 ? &cv2MarchPipeLayout : &cv2ResolvePipeLayout);
    }
    cv2MarchPipeline = makeComputePipeline(ctx, "shaders/cloud_v2_march.comp.spv", cv2MarchPipeLayout);
    cv2ResolvePipeline = makeComputePipeline(ctx, "shaders/cloud_v2_resolve.comp.spv", cv2ResolvePipeLayout);

    // Static descriptors of the march set (the screen-sized ones are written by the targets).
    {
        VkDescriptorBufferInfo cpInfo{cloudParamsBuf, 0, sizeof(GpuCloudParams)};
        VkDescriptorBufferInfo v2Info{cv2ParamsBuf, 0, sizeof(GpuCloudV2Params)};
        VkDescriptorImageInfo weather{cv2RepeatSampler, cv2WeatherView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkDescriptorImageInfo shape{cv2RepeatSampler, cv2ShapeView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkDescriptorImageInfo detail{cv2RepeatSampler, cv2DetailView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkDescriptorImageInfo meso{cv2RepeatSampler, cv2MesoView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkDescriptorImageInfo night{earthNightSampler ? earthNightSampler : noiseSampler,
                                    earthNightView ? earthNightView : noiseTexView,
                                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkDescriptorBufferInfo terr{terrainFrameBuf, 0, VK_WHOLE_SIZE};
        VkDescriptorBufferInfo beams{beamCloudLightBuf, 0, VK_WHOLE_SIZE};
        VkWriteDescriptorSet w[] = {
            bufferWrite(cv2MarchDescSet, 0, UBO, &cpInfo),
            bufferWrite(cv2MarchDescSet, 1, UBO, &v2Info),
            imageWrite(cv2MarchDescSet, 2, TEX, &weather),
            imageWrite(cv2MarchDescSet, 3, TEX, &shape),
            imageWrite(cv2MarchDescSet, 4, TEX, &detail),
            imageWrite(cv2MarchDescSet, 5, TEX, &meso),
            imageWrite(cv2MarchDescSet, 7, TEX, &night),
            bufferWrite(cv2MarchDescSet, 8, SSBO, &terr),
            bufferWrite(cv2MarchDescSet, 9, SSBO, &beams),
            bufferWrite(cv2ResolveDescSet, 0, UBO, &v2Info)};
        vkUpdateDescriptorSets(dev, (uint32_t)(sizeof(w) / sizeof(w[0])), w, 0, nullptr);
    }

    createCloudsV2Targets(ctx);
    Log::line("clouds v2: resources ready");
}

// ─── Swapchain-sized targets ──────────────────────────────────────────────────────────────────────
void SatelliteSim::createCloudsV2Targets(VulkanContext &ctx)
{
    VkDevice dev = ctx.device;
    cv2HalfW = (ctx.swapExtent.width + 1) / 2;
    cv2HalfH = (ctx.swapExtent.height + 1) / 2;
    cv2QuarterW = (cv2HalfW + 1) / 2;
    cv2QuarterH = (cv2HalfH + 1) / 2;

    auto make = [&](uint32_t w, uint32_t h, VkFormat fmt, VkImageUsageFlags extra,
                    VkImage &img, VkDeviceMemory &mem, VkImageView &view)
    {
        ctx.createImage(w, h, fmt, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | extra, img, mem);
        view = makeView(dev, img, VK_IMAGE_VIEW_TYPE_2D, fmt, 0, 1, 1);
    };
    make(cv2QuarterW, cv2QuarterH, VK_FORMAT_R16G16B16A16_SFLOAT, 0, cv2NewImg, cv2NewMem, cv2NewView);
    make(cv2QuarterW, cv2QuarterH, VK_FORMAT_R32G32_SFLOAT, 0, cv2NewDepthImg, cv2NewDepthMem, cv2NewDepthView);
    make(cv2HalfW, cv2HalfH, VK_FORMAT_R16G16B16A16_SFLOAT,
         VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, // copied from; cleared at creation
         cv2ResolvedImg, cv2ResolvedMem, cv2ResolvedView);
    make(cv2HalfW, cv2HalfH, VK_FORMAT_R32G32_SFLOAT, VK_IMAGE_USAGE_TRANSFER_DST_BIT,
         cv2ResolvedDepthImg, cv2ResolvedDepthMem, cv2ResolvedDepthView);
    make(cv2HalfW, cv2HalfH, VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_USAGE_TRANSFER_DST_BIT,
         cv2HistoryImg, cv2HistoryMem, cv2HistoryView);

    VkCommandBuffer cmd = ctx.beginOneTimeCommands();
    for (VkImage img : {cv2NewImg, cv2NewDepthImg, cv2ResolvedImg, cv2ResolvedDepthImg, cv2HistoryImg})
        ctx.imageBarrier(cmd, img, 0, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
                         VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                         VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    // Resolved starts transparent-and-empty (T = 1), so cloud_march reads "no cloud" if v2 is
    // switched on before its first frame has run.
    VkClearColorValue clr{};
    clr.float32[3] = 1.0f;
    VkImageSubresourceRange rng{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(cmd, cv2ResolvedImg, VK_IMAGE_LAYOUT_GENERAL, &clr, 1, &rng);
    VkClearColorValue zero{};
    vkCmdClearColorImage(cmd, cv2ResolvedDepthImg, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &rng);
    ctx.endOneTimeCommands(cmd);

    VkDescriptorImageInfo depthInfo{sceneDepthSampler, sceneDepthView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkDescriptorImageInfo newOut{VK_NULL_HANDLE, cv2NewView, VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorImageInfo newDepthOut{VK_NULL_HANDLE, cv2NewDepthView, VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorImageInfo newIn{cv2ClampSampler, cv2NewView, VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorImageInfo newDepthIn{cv2ClampSampler, cv2NewDepthView, VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorImageInfo histIn{cv2ClampSampler, cv2HistoryView, VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorImageInfo resOut{VK_NULL_HANDLE, cv2ResolvedView, VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorImageInfo resDepthOut{VK_NULL_HANDLE, cv2ResolvedDepthView, VK_IMAGE_LAYOUT_GENERAL};
    const VkDescriptorType TEX = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, IMG = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    VkWriteDescriptorSet w[] = {
        imageWrite(cv2MarchDescSet, 6, TEX, &depthInfo),
        imageWrite(cv2MarchDescSet, 10, IMG, &newOut),
        imageWrite(cv2MarchDescSet, 11, IMG, &newDepthOut),
        imageWrite(cv2ResolveDescSet, 1, TEX, &newIn),
        imageWrite(cv2ResolveDescSet, 2, TEX, &newDepthIn),
        imageWrite(cv2ResolveDescSet, 3, TEX, &histIn),
        imageWrite(cv2ResolveDescSet, 4, IMG, &resOut),
        imageWrite(cv2ResolveDescSet, 5, IMG, &resDepthOut)};
    vkUpdateDescriptorSets(dev, (uint32_t)(sizeof(w) / sizeof(w[0])), w, 0, nullptr);
    cv2HistoryValid = false;
}

void SatelliteSim::destroyCloudsV2Targets(VkDevice device)
{
    VkImageView *views[] = {&cv2NewView, &cv2NewDepthView, &cv2ResolvedView, &cv2ResolvedDepthView, &cv2HistoryView};
    VkImage *imgs[] = {&cv2NewImg, &cv2NewDepthImg, &cv2ResolvedImg, &cv2ResolvedDepthImg, &cv2HistoryImg};
    VkDeviceMemory *mems[] = {&cv2NewMem, &cv2NewDepthMem, &cv2ResolvedMem, &cv2ResolvedDepthMem, &cv2HistoryMem};
    for (int i = 0; i < 5; ++i)
    {
        if (*views[i]) vkDestroyImageView(device, *views[i], nullptr);
        if (*imgs[i]) vkDestroyImage(device, *imgs[i], nullptr);
        if (*mems[i]) vkFreeMemory(device, *mems[i], nullptr);
        *views[i] = VK_NULL_HANDLE;
        *imgs[i] = VK_NULL_HANDLE;
        *mems[i] = VK_NULL_HANDLE;
    }
}

void SatelliteSim::destroyCloudsV2(VkDevice device)
{
    destroyCloudsV2Targets(device);
    if (cv2MarchPipeline) vkDestroyPipeline(device, cv2MarchPipeline, nullptr);
    if (cv2ResolvePipeline) vkDestroyPipeline(device, cv2ResolvePipeline, nullptr);
    if (cv2MarchPipeLayout) vkDestroyPipelineLayout(device, cv2MarchPipeLayout, nullptr);
    if (cv2ResolvePipeLayout) vkDestroyPipelineLayout(device, cv2ResolvePipeLayout, nullptr);
    if (cv2DescPool) vkDestroyDescriptorPool(device, cv2DescPool, nullptr);
    if (cv2MarchDescLayout) vkDestroyDescriptorSetLayout(device, cv2MarchDescLayout, nullptr);
    if (cv2ResolveDescLayout) vkDestroyDescriptorSetLayout(device, cv2ResolveDescLayout, nullptr);
    cv2MarchPipeline = cv2ResolvePipeline = VK_NULL_HANDLE;
    cv2MarchPipeLayout = cv2ResolvePipeLayout = VK_NULL_HANDLE;
    cv2DescPool = VK_NULL_HANDLE;
    cv2MarchDescLayout = cv2ResolveDescLayout = VK_NULL_HANDLE;
    if (cv2ParamsBuf) vkDestroyBuffer(device, cv2ParamsBuf, nullptr);
    if (cv2ParamsMem) vkFreeMemory(device, cv2ParamsMem, nullptr);
    cv2ParamsBuf = VK_NULL_HANDLE;
    cv2ParamsMem = VK_NULL_HANDLE;
    cv2ParamsMapped = nullptr;
    VkImageView *views[] = {&cv2WeatherView, &cv2ShapeView, &cv2DetailView, &cv2MesoView};
    VkImage *imgs[] = {&cv2WeatherImg, &cv2ShapeImg, &cv2DetailImg, &cv2MesoImg};
    VkDeviceMemory *mems[] = {&cv2WeatherMem, &cv2ShapeMem, &cv2DetailMem, &cv2MesoMem};
    for (int i = 0; i < 4; ++i)
    {
        if (*views[i]) vkDestroyImageView(device, *views[i], nullptr);
        if (*imgs[i]) vkDestroyImage(device, *imgs[i], nullptr);
        if (*mems[i]) vkFreeMemory(device, *mems[i], nullptr);
        *views[i] = VK_NULL_HANDLE;
        *imgs[i] = VK_NULL_HANDLE;
        *mems[i] = VK_NULL_HANDLE;
    }
    if (cv2RepeatSampler) vkDestroySampler(device, cv2RepeatSampler, nullptr);
    if (cv2ClampSampler) vkDestroySampler(device, cv2ClampSampler, nullptr);
    cv2RepeatSampler = cv2ClampSampler = VK_NULL_HANDLE;
}

// cloud_march.comp bindings 15-20 and beam_self_march.comp bindings 5-8. Called once both sets
// exist (init) and again after a resize (19/20 point at the recreated resolved images).
void SatelliteSim::writeCloudsV2ConsumerDescriptors(VulkanContext &ctx)
{
    const VkDescriptorType UBO = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, TEX = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    VkDescriptorBufferInfo v2Info{cv2ParamsBuf, 0, sizeof(GpuCloudV2Params)};
    VkDescriptorImageInfo weather{cv2RepeatSampler, cv2WeatherView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkDescriptorImageInfo shape{cv2RepeatSampler, cv2ShapeView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkDescriptorImageInfo meso{cv2RepeatSampler, cv2MesoView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkDescriptorImageInfo res{cv2ClampSampler, cv2ResolvedView, VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorImageInfo resDepth{cv2ClampSampler, cv2ResolvedDepthView, VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet w[] = {
        bufferWrite(cloudMarchDescSet, 15, UBO, &v2Info),
        imageWrite(cloudMarchDescSet, 16, TEX, &weather),
        imageWrite(cloudMarchDescSet, 17, TEX, &shape),
        imageWrite(cloudMarchDescSet, 18, TEX, &meso),
        imageWrite(cloudMarchDescSet, 19, TEX, &res),
        imageWrite(cloudMarchDescSet, 20, TEX, &resDepth),
        bufferWrite(beamSelfMarchDescSet, 5, UBO, &v2Info),
        imageWrite(beamSelfMarchDescSet, 6, TEX, &weather),
        imageWrite(beamSelfMarchDescSet, 7, TEX, &shape),
        imageWrite(beamSelfMarchDescSet, 8, TEX, &meso)};
    vkUpdateDescriptorSets(ctx.device, (uint32_t)(sizeof(w) / sizeof(w[0])), w, 0, nullptr);
}

// ─── Per frame ────────────────────────────────────────────────────────────────────────────────────
// Filled every frame, v2 on or off: beam_self_march.comp reads cv2.shell while cloudsV2 is set,
// and the harness can switch v2 on between any two frames.
void SatelliteSim::fillCloudsV2Params(VulkanContext &ctx, const CloudMarchPC &cpc)
{
    if (!cv2ParamsMapped)
        return;
    GpuCloudV2Params p{};

    const double R = 6371000.0;
    const glm::dvec3 up = glm::normalize(glm::dvec3(cpc.obsECEFDir));
    // The GPU's own ground under the observer (terrainFrameBuf, one frame old) when it has one —
    // the eye every v2 pass builds its rays from is terrainFrame.x + 2.
    const double groundGpu = terrainFrameMapped ? (double)terrainFrameMapped[0] : 0.0;
    const double eyeH = (groundGpu > 0.0 ? groundGpu : (double)cpc.obsEffH) + 2.0;
    const glm::dvec3 eye = up * (R + eyeH);
    const double simT = (double)simDayJ2000 * 86400.0 + simSecInDay;
    const float tanHalf = std::tan(cpc.fovYRad * 0.5f);

    // Settings that change what a pixel shows invalidate the history.
    uint64_t h = 1469598103934665603ull;
    auto mix = [&](float v)
    {
        uint32_t b;
        std::memcpy(&b, &v, 4);
        h = (h ^ b) * 1099511628211ull;
    };
    for (float v : {cv2Coverage, cv2Density, cv2Detail, cv2AmbientGain, cv2SunGain, cv2BounceGain, cv2Powder,
                    cv2LightLenM, (float)cv2LightSteps, cv2StepBaseM, cv2StepGrowth, cv2StepMaxM, cv2MaxDistKm,
                    cv2ShapePeriodM, cv2DetailPeriodM, cv2CellPeriodM, cv2ClusterPeriodM, (float)cv2DebugView,
                    cv2EdgeSharpness, cv2WeatherWarpKm})
        mix(v);
    for (const GpuCloudV2Type &t : cv2Types)
        for (int k = 0; k < 4; ++k)
        {
            mix(t.alt[k]);
            mix(t.shape[k]);
            mix(t.look[k]);
        }

    bool valid = cv2HistoryValid && cloudsV2Enabled && h == cv2SettingsHash &&
                 glm::length(eye - cv2PrevEye) < 20000.0 && std::abs(simT - cv2PrevSimT) < 600.0;

    p.prevSkyView = cv2PrevSkyView;
    p.prevObs = glm::vec4(cv2PrevObsDir, cv2PrevTanHalf);
    p.obsDelta = glm::vec4(glm::vec3(eye - cv2PrevEye), cv2PrevAspect);

    // Noise anchors: the observer's SEA-LEVEL point (what the shaders measure from) plus the wind,
    // in periods, reduced in double. Each volume drifts at its own rate, so the shape moves through
    // the cells and the detail through the shape: the clouds evolve instead of sliding as one.
    const glm::dvec3 sea = up * R;
    const glm::dvec3 windDir = glm::normalize(glm::dvec3(0.83, 0.52, 0.19));
    auto anchor = [&](double periodM, double windMul)
    {
        const glm::dvec3 a = (sea + windDir * ((double)cv2WindMps * windMul * simT)) / periodM;
        return glm::vec4((float)fracPos(a.x), (float)fracPos(a.y), (float)fracPos(a.z), (float)(1.0 / periodM));
    };
    p.anchorShape = anchor(cv2ShapePeriodM, 1.0);
    p.anchorDetail = anchor(cv2DetailPeriodM, 1.6);
    p.anchorCluster = anchor(cv2ClusterPeriodM, 0.6);
    p.anchorCell = anchor(cv2CellPeriodM, 0.85);

    static const int kOffsets[4][2] = {{0, 0}, {1, 1}, {1, 0}, {0, 1}};
    const int *o = kOffsets[cv2Frame & 3u];
    p.frame = glm::vec4((float)(cv2Frame & 0xFFFFu), valid ? 1.0f : 0.0f, (float)o[0], (float)o[1]);
    p.march = glm::vec4(cv2StepBaseM, cv2StepGrowth, cv2StepMaxM, cv2MaxDistKm * 1000.0f);
    p.light = glm::vec4(cv2LightLenM, (float)std::clamp(cv2LightSteps, 1, 12), 0.35f, 0.5f);
    p.look = glm::vec4(cv2Coverage, cv2Density, cv2Detail, cv2AmbientGain);
    p.look2 = glm::vec4(cv2BounceGain, cv2SunGain, cv2Powder, cv2HistoryWeight);
    p.phase = glm::vec4(0.8f, 0.6f, -0.25f, 0.4f);

    float lo = 1e9f, hi = 0.0f;
    for (int i = 0; i < kCloudV2Types; ++i)
    {
        p.types[i] = cv2Types[i];
        lo = std::min(lo, cv2Types[i].alt.x - cv2Types[i].alt.w);
        hi = std::max(hi, cv2Types[i].alt.x + (cv2Types[i].alt.y - cv2Types[i].alt.x) * 1.35f);
    }
    p.shell = glm::vec4(std::max(lo, 0.0f), hi, (float)cv2DebugView, cv2DetailLodStartM);
    p.extra = glm::vec4(cv2EdgeSharpness, cv2WeatherWarpKm * 1000.0f / (float)R, 0.0f, 0.0f);
    std::memcpy(cv2ParamsMapped, &p, sizeof(p));

    // This frame becomes the next frame's "previous" (only when v2 actually drew it).
    if (cloudsV2Enabled)
    {
        cv2PrevSkyView = cpc.skyView;
        cv2PrevObsDir = glm::vec3(up);
        cv2PrevEye = eye;
        cv2PrevTanHalf = tanHalf;
        cv2PrevAspect = cpc.aspect;
        cv2PrevSimT = simT;
        cv2SettingsHash = h;
        cv2HistoryValid = true;
        ++cv2Frame;
    }
    else
        cv2HistoryValid = false;
    (void)ctx;
}

void SatelliteSim::recordCloudsV2(VkCommandBuffer cmd, VulkanContext &ctx, const CloudMarchPC &cpc)
{
    fillCloudsV2Params(ctx, cpc);
    if (!cloudsV2Enabled || !cv2MarchPipeline)
        return;

    // scene_depth.comp wrote terrainFrameBuf (the eye height) this frame.
    memoryBarrier(cmd, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                  VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, cv2MarchPipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, cv2MarchPipeLayout, 0, 1, &cv2MarchDescSet, 0, nullptr);
    vkCmdPushConstants(cmd, cv2MarchPipeLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(cpc), &cpc);
    vkCmdDispatch(cmd, (cv2QuarterW + 15) / 16, (cv2QuarterH + 15) / 16, 1);

    memoryBarrier(cmd, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                  VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, cv2ResolvePipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, cv2ResolvePipeLayout, 0, 1, &cv2ResolveDescSet, 0, nullptr);
    vkCmdPushConstants(cmd, cv2ResolvePipeLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(cpc), &cpc);
    vkCmdDispatch(cmd, (cv2HalfW + 15) / 16, (cv2HalfH + 15) / 16, 1);

    // Resolved -> cloud_march (compute read) and -> history (copy).
    memoryBarrier(cmd, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT,
                  VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                  VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkImageCopy region{};
    region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.extent = {cv2HalfW, cv2HalfH, 1};
    vkCmdCopyImage(cmd, cv2ResolvedImg, VK_IMAGE_LAYOUT_GENERAL, cv2HistoryImg, VK_IMAGE_LAYOUT_GENERAL, 1, &region);
    memoryBarrier(cmd, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                  VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
}
