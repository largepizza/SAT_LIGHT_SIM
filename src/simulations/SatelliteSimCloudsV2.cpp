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

// wg: the workgroup size as specialization constants 0/1 (a shader declaring local_size_x_id /
// local_size_y_id); null = the shader's own.
VkPipeline makeComputePipeline(VulkanContext &ctx, const char *spv, VkPipelineLayout layout,
                               const uint32_t *wg = nullptr, uint32_t nSpec = 2)
{
    VkShaderModule mod = ctx.loadShader(spv);
    VkPipelineShaderStageCreateInfo stage{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    stage.module = mod;
    stage.pName = "main";
    VkSpecializationMapEntry wgMap[3] = {{0, 0, 4}, {1, 4, 4}, {2, 8, 4}};
    VkSpecializationInfo wgSpec{nSpec, wgMap, nSpec * 4u, wg};
    if (wg)
        stage.pSpecializationInfo = &wgSpec;
    VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    ci.stage = stage;
    ci.layout = layout;
    if (ctx.pipelineStatsSupported)
        ci.flags |= VK_PIPELINE_CREATE_CAPTURE_STATISTICS_BIT_KHR;
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

// A 64x64 tileable blue-noise tile by void and cluster (Ulichney 1993): 8-bit ranks, row-major. The
// march's ray jitter in fast flight: its error sits at high spatial frequencies, which the resolve's 3x3
// motion filter removes (white noise leaves clumps; IGN's smooth diagonals lined the steps up into bands).
static std::vector<uint8_t> makeBlueNoise64()
{
    const int N = 64, NN = N * N;
    std::vector<float> ker(NN);
    for (int y = 0; y < N; ++y)
        for (int x = 0; x < N; ++x)
        {
            const int dx = std::min(x, N - x), dy = std::min(y, N - y);
            ker[y * N + x] = std::exp(-(float)(dx * dx + dy * dy) / (2.0f * 1.5f * 1.5f));
        }
    auto splat = [&](std::vector<float> &E, int p, float s) {
        const int px = p % N, py = p / N;
        for (int y = 0; y < N; ++y)
        {
            const float *kr = &ker[((y - py + N) & (N - 1)) * N];
            float *er = &E[y * N];
            for (int x = 0; x < N; ++x)
                er[x] += s * kr[(x - px + N) & (N - 1)];
        }
    };
    auto extreme = [&](const std::vector<float> &E, const std::vector<uint8_t> &b, uint8_t want, bool mx) {
        int best = -1;
        for (int i = 0; i < NN; ++i)
            if (b[i] == want && (best < 0 || (mx ? E[i] > E[best] : E[i] < E[best])))
                best = i;
        return best;
    };
    // The initial pattern: a tenth of the cells, then relaxed until the tightest cluster IS the largest void.
    std::vector<uint8_t> bits(NN, 0);
    std::vector<float> E(NN, 0.0f);
    uint32_t rng = 0x9E3779B9u;
    int ones = 0;
    while (ones < NN / 10)
    {
        rng = rng * 1664525u + 1013904223u;
        const int p = (int)((rng >> 8) % (uint32_t)NN);
        if (bits[p]) continue;
        bits[p] = 1; splat(E, p, 1.0f); ++ones;
    }
    for (int it = 0; it < NN; ++it)
    {
        const int c = extreme(E, bits, 1, true);
        bits[c] = 0; splat(E, c, -1.0f);
        const int v = extreme(E, bits, 0, false);
        bits[v] = 1; splat(E, v, 1.0f);
        if (v == c) break;
    }
    std::vector<int> rank(NN, 0);
    {   // Ranks below the initial count: remove the tightest cluster, one at a time.
        std::vector<uint8_t> b = bits;
        std::vector<float> e = E;
        for (int r = ones - 1; r >= 0; --r)
        {
            const int c = extreme(e, b, 1, true);
            rank[c] = r; b[c] = 0; splat(e, c, -1.0f);
        }
    }
    // Ranks above it: fill the largest void, one at a time.
    for (int r = ones; r < NN; ++r)
    {
        const int v = extreme(E, bits, 0, false);
        rank[v] = r; bits[v] = 1; splat(E, v, 1.0f);
    }
    std::vector<uint8_t> out(NN);
    for (int i = 0; i < NN; ++i)
        out[i] = (uint8_t)((rank[i] * 256) / NN);
    return out;
}

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
                                                       VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                                                       VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER});
        VkPushConstantRange pcr{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(GpuWeatherPC)};
        VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pli.setLayoutCount = 1;
        pli.pSetLayouts = &bl;
        pli.pushConstantRangeCount = 1;
        pli.pPushConstantRanges = &pcr;
        VkPipelineLayout pl = VK_NULL_HANDLE;
        vkCreatePipelineLayout(dev, &pli, nullptr, &pl);
        VkPipeline pipe = makeComputePipeline(ctx, "shaders/cloud_v2_weather.comp.spv", pl);

        VkDescriptorPoolSize ps[2] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 3 * kCv2WeatherMips},
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
        // The DEM (ground height in the alpha: cloud bases follow the terrain). Without it the noise
        // texture stands in, which reads as low hills — acceptable for a missing-asset fallback.
        VkDescriptorImageInfo elevInfo{earthElevSampler ? earthElevSampler : noiseSampler,
                                       earthElevView ? earthElevView : noiseTexView,
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
            VkWriteDescriptorSet w[4] = {
                imageWrite(set, 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &cloudsInfo),
                imageWrite(set, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &specInfo),
                imageWrite(set, 2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &outInfo),
                imageWrite(set, 3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &elevInfo)};
            vkUpdateDescriptorSets(dev, 4, w, 0, nullptr);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 1, &set, 0, nullptr);
            cv2WxSets.push_back(set);
            GpuWeatherPC pcv = weatherEvoPC(-1, m);
            vkCmdPushConstants(cmd, pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pcv), &pcv);
            uint32_t g = std::max(1u, ((kCv2WeatherFace >> m) + 7) / 8);
            vkCmdDispatch(cmd, g, g, 6);
        }
        (void)srcLod0;
        transitionAll(cmd, cv2WeatherImg, kCv2WeatherMips, 6, VK_IMAGE_LAYOUT_GENERAL,
                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                      VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                      VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
        ctx.endOneTimeCommands(cmd);
        // Kept for the evolution's re-bakes (recordWeatherEvolution); destroyed with the rest.
        cv2WxMipViews = mipViews;
        cv2WxPipeline = pipe;
        cv2WxPipeLayout = pl;
        cv2WxPool = pool;
        cv2WxSetLayout = bl;
        const double t0 = (double)simDayJ2000 * 86400.0 + simSecInDay;
        for (double &b : cv2WxBakedT)
            b = t0;
        cv2WxHash = std::hash<float>{}(cv2EvoWindMps) ^ (std::hash<float>{}(cv2EvoGrowth) * 31u)
                  ^ (std::hash<float>{}(cv2EvoWindowH) * 131u) ^ (std::hash<float>{}(cv2EvoDiurnal) * 1031u);
    }

    // ── UBO ──
    ctx.createBuffer(sizeof(GpuCloudV2Params), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                     cv2ParamsBuf, cv2ParamsMem);
    vkMapMemory(dev, cv2ParamsMem, 0, sizeof(GpuCloudV2Params), 0, &cv2ParamsMapped);
    std::memset(cv2ParamsMapped, 0, sizeof(GpuCloudV2Params));
    ctx.createBuffer(kCv2FlashBufBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                     cv2FlashBuf, cv2FlashMem);
    vkMapMemory(dev, cv2FlashMem, 0, kCv2FlashBufBytes, 0, &cv2FlashMapped);
    std::memset(cv2FlashMapped, 0, kCv2FlashBufBytes);
    ctx.createBuffer(16 + 8 * kCv2MaxTiles, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT
                                                 | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                     VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, cv2TileBuf, cv2TileMem);
    {
        const std::vector<uint8_t> bn = makeBlueNoise64();
        ctx.createBuffer(bn.size(), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                         cv2BlueNoiseBuf, cv2BlueNoiseMem);
        void *m = nullptr;
        vkMapMemory(dev, cv2BlueNoiseMem, 0, bn.size(), 0, &m);
        std::memcpy(m, bn.data(), bn.size());
        vkUnmapMemory(dev, cv2BlueNoiseMem);
    }
    {
        VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ci.imageType = VK_IMAGE_TYPE_3D;
        ci.format = VK_FORMAT_R16_SFLOAT;
        ci.extent = {kCv2LightVolXY, kCv2LightVolXY, kCv2LightVolZ};
        ci.mipLevels = 1;
        ci.arrayLayers = 1;
        ci.samples = VK_SAMPLE_COUNT_1_BIT;
        ci.tiling = VK_IMAGE_TILING_OPTIMAL;
        ci.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (vkCreateImage(dev, &ci, nullptr, &cv2LightVolImg) != VK_SUCCESS)
            throw std::runtime_error("clouds v2: light volume vkCreateImage failed");
        VkMemoryRequirements req;
        vkGetImageMemoryRequirements(dev, cv2LightVolImg, &req);
        VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        mai.allocationSize = req.size;
        mai.memoryTypeIndex = ctx.findMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        vkAllocateMemory(dev, &mai, nullptr, &cv2LightVolMem);
        vkBindImageMemory(dev, cv2LightVolImg, cv2LightVolMem, 0);
        cv2LightVolView = makeView(dev, cv2LightVolImg, VK_IMAGE_VIEW_TYPE_3D, VK_FORMAT_R16_SFLOAT, 0, 1, 1);
        // GENERAL for good (written by the bake, sampled by the march), cleared to full transmittance so
        // the levels not yet baked shadow nothing.
        VkCommandBuffer cmd = ctx.beginOneTimeCommands();
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = cv2LightVolImg;
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr,
                             0, nullptr, 1, &b);
        VkClearColorValue one{};
        one.float32[0] = 1.0f;
        vkCmdClearColorImage(cmd, cv2LightVolImg, VK_IMAGE_LAYOUT_GENERAL, &one, 1, &b.subresourceRange);
        b.oldLayout = b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr,
                             0, nullptr, 1, &b);
        ctx.endOneTimeCommands(cmd);
    }

    // ── Pass layouts, sets, pipelines ──
    using T = VkDescriptorType;
    const T UBO = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, TEX = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            SSBO = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, IMG = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    // cloud_v2_march.comp: 0 CloudParams, 1 CloudV2Params, 2 weather, 3 shape, 4 detail, 5 meso,
    // 6 sceneDepth, 7 earthNight, 8 terrainFrame, 9 beamCloudLights, 10 outColor, 11 outDepth,
    // 12 the lightning flash list (cloud_v2_lightning.comp, same set), 13 the light volume (storage: the
    // bake, cloud_v2_lightvol.comp), 14 the light volume (sampled: the march's godrays), 15 the blue-noise
    // tile (the ray jitter in fast flight); the adaptive rate: 16 last frame's resolved depth, 17/18 pass B's full-rate
    // targets, 19 the tile list
    cv2MarchDescLayout = makeSetLayout(dev, {UBO, UBO, TEX, TEX, TEX, TEX, TEX, TEX, SSBO, SSBO, IMG, IMG, SSBO, IMG, TEX, SSBO,
                                             TEX, IMG, IMG, SSBO});
    // cloud_v2_resolve.comp: 0 CloudV2Params, 1 new color, 2 new depth, 3 history, 4 out color, 5 out depth,
    // 6/7 the full-rate tiles' color/depth, 8 the tile list
    cv2ResolveDescLayout = makeSetLayout(dev, {UBO, TEX, TEX, TEX, IMG, IMG, TEX, TEX, SSBO});
    {
        VkDescriptorPoolSize ps[4] = {{UBO, 3}, {TEX, 13}, {SSBO, 6}, {IMG, 7}};
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
    cv2MarchPipeline = makeComputePipeline(ctx, "shaders/cloud_v2_march.comp.spv", cv2MarchPipeLayout, cv2MarchWg);
    {
        const uint32_t specB[3] = {cv2MarchWg[0], cv2MarchWg[1], 1u};
        cv2MarchPassBPipeline = makeComputePipeline(ctx, "shaders/cloud_v2_march.comp.spv", cv2MarchPipeLayout, specB, 3);
    }
    cv2TilesPipeline = makeComputePipeline(ctx, "shaders/cloud_v2_tiles.comp.spv", cv2MarchPipeLayout);
    cv2ResolvePipeline = makeComputePipeline(ctx, "shaders/cloud_v2_resolve.comp.spv", cv2ResolvePipeLayout);
    cv2LightningPipeline = makeComputePipeline(ctx, "shaders/cloud_v2_lightning.comp.spv", cv2MarchPipeLayout);
    cv2LightVolPipeline = makeComputePipeline(ctx, "shaders/cloud_v2_lightvol.comp.spv", cv2MarchPipeLayout);

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
        VkDescriptorBufferInfo flashes{cv2FlashBuf, 0, VK_WHOLE_SIZE};
        VkDescriptorBufferInfo blueNoise{cv2BlueNoiseBuf, 0, VK_WHOLE_SIZE};
        VkDescriptorBufferInfo tiles{cv2TileBuf, 0, VK_WHOLE_SIZE};
        VkDescriptorImageInfo lvOut{VK_NULL_HANDLE, cv2LightVolView, VK_IMAGE_LAYOUT_GENERAL};
        VkDescriptorImageInfo lvIn{cv2ClampSampler, cv2LightVolView, VK_IMAGE_LAYOUT_GENERAL};
        VkWriteDescriptorSet w[] = {
            bufferWrite(cv2MarchDescSet, 15, SSBO, &blueNoise),
            bufferWrite(cv2MarchDescSet, 19, SSBO, &tiles),
            bufferWrite(cv2ResolveDescSet, 8, SSBO, &tiles),
            imageWrite(cv2MarchDescSet, 13, IMG, &lvOut),
            imageWrite(cv2MarchDescSet, 14, TEX, &lvIn),
            bufferWrite(cv2MarchDescSet, 0, UBO, &cpInfo),
            bufferWrite(cv2MarchDescSet, 1, UBO, &v2Info),
            imageWrite(cv2MarchDescSet, 2, TEX, &weather),
            imageWrite(cv2MarchDescSet, 3, TEX, &shape),
            imageWrite(cv2MarchDescSet, 4, TEX, &detail),
            imageWrite(cv2MarchDescSet, 5, TEX, &meso),
            imageWrite(cv2MarchDescSet, 7, TEX, &night),
            bufferWrite(cv2MarchDescSet, 8, SSBO, &terr),
            bufferWrite(cv2MarchDescSet, 9, SSBO, &beams),
            bufferWrite(cv2MarchDescSet, 12, SSBO, &flashes),
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
    // The march's own targets are half-res sized: a sparse frame fills only their quarter-size
    // top-left region, a full-rate frame (from altitude, see fillCloudsV2Params) all of it.
    make(cv2HalfW, cv2HalfH, VK_FORMAT_R16G16B16A16_SFLOAT, 0, cv2NewImg, cv2NewMem, cv2NewView);
    make(cv2HalfW, cv2HalfH, VK_FORMAT_R32G32_SFLOAT, 0, cv2NewDepthImg, cv2NewDepthMem, cv2NewDepthView);
    make(cv2HalfW, cv2HalfH, VK_FORMAT_R16G16B16A16_SFLOAT,
         VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, // copied from; cleared at creation
         cv2ResolvedImg, cv2ResolvedMem, cv2ResolvedView);
    make(cv2HalfW, cv2HalfH, VK_FORMAT_R32G32_SFLOAT, VK_IMAGE_USAGE_TRANSFER_DST_BIT,
         cv2ResolvedDepthImg, cv2ResolvedDepthMem, cv2ResolvedDepthView);
    make(cv2HalfW, cv2HalfH, VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_USAGE_TRANSFER_DST_BIT,
         cv2HistoryImg, cv2HistoryMem, cv2HistoryView);
    make(cv2HalfW, cv2HalfH, VK_FORMAT_R16G16B16A16_SFLOAT, 0, cv2FullImg, cv2FullMem, cv2FullView);
    make(cv2HalfW, cv2HalfH, VK_FORMAT_R32G32_SFLOAT, 0, cv2FullDepthImg, cv2FullDepthMem, cv2FullDepthView);

    VkCommandBuffer cmd = ctx.beginOneTimeCommands();
    for (VkImage img : {cv2NewImg, cv2NewDepthImg, cv2ResolvedImg, cv2ResolvedDepthImg, cv2HistoryImg, cv2FullImg, cv2FullDepthImg})
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
    VkDescriptorImageInfo resDepthIn{cv2ClampSampler, cv2ResolvedDepthView, VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorImageInfo fullOut{VK_NULL_HANDLE, cv2FullView, VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorImageInfo fullDepthOut{VK_NULL_HANDLE, cv2FullDepthView, VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorImageInfo fullIn{cv2ClampSampler, cv2FullView, VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorImageInfo fullDepthIn{cv2ClampSampler, cv2FullDepthView, VK_IMAGE_LAYOUT_GENERAL};
    const VkDescriptorType TEX = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, IMG = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    VkWriteDescriptorSet w[] = {
        imageWrite(cv2MarchDescSet, 6, TEX, &depthInfo),
        imageWrite(cv2MarchDescSet, 10, IMG, &newOut),
        imageWrite(cv2MarchDescSet, 11, IMG, &newDepthOut),
        imageWrite(cv2ResolveDescSet, 1, TEX, &newIn),
        imageWrite(cv2ResolveDescSet, 2, TEX, &newDepthIn),
        imageWrite(cv2ResolveDescSet, 3, TEX, &histIn),
        imageWrite(cv2ResolveDescSet, 4, IMG, &resOut),
        imageWrite(cv2ResolveDescSet, 5, IMG, &resDepthOut),
        imageWrite(cv2MarchDescSet, 16, TEX, &resDepthIn),
        imageWrite(cv2MarchDescSet, 17, IMG, &fullOut),
        imageWrite(cv2MarchDescSet, 18, IMG, &fullDepthOut),
        imageWrite(cv2ResolveDescSet, 6, TEX, &fullIn),
        imageWrite(cv2ResolveDescSet, 7, TEX, &fullDepthIn)};
    vkUpdateDescriptorSets(dev, (uint32_t)(sizeof(w) / sizeof(w[0])), w, 0, nullptr);
    cv2HistoryValid = false;
}

void SatelliteSim::destroyCloudsV2Targets(VkDevice device)
{
    VkImageView *views[] = {&cv2NewView, &cv2NewDepthView, &cv2ResolvedView, &cv2ResolvedDepthView, &cv2HistoryView,
                            &cv2FullView, &cv2FullDepthView};
    VkImage *imgs[] = {&cv2NewImg, &cv2NewDepthImg, &cv2ResolvedImg, &cv2ResolvedDepthImg, &cv2HistoryImg,
                       &cv2FullImg, &cv2FullDepthImg};
    VkDeviceMemory *mems[] = {&cv2NewMem, &cv2NewDepthMem, &cv2ResolvedMem, &cv2ResolvedDepthMem, &cv2HistoryMem,
                              &cv2FullMem, &cv2FullDepthMem};
    for (int i = 0; i < 7; ++i)
    {
        if (*views[i]) vkDestroyImageView(device, *views[i], nullptr);
        if (*imgs[i]) vkDestroyImage(device, *imgs[i], nullptr);
        if (*mems[i]) vkFreeMemory(device, *mems[i], nullptr);
        *views[i] = VK_NULL_HANDLE;
        *imgs[i] = VK_NULL_HANDLE;
        *mems[i] = VK_NULL_HANDLE;
    }
}

// The driver's statistics for a compute pipeline ("Register Count 128, ..."), or "" when the device
// has no VK_KHR_pipeline_executable_properties.
static std::string pipelineStatsLine(VulkanContext &ctx, VkPipeline p)
{
    if (!ctx.pipelineStatsSupported || !p) return "";
    auto getProps = (PFN_vkGetPipelineExecutablePropertiesKHR)vkGetDeviceProcAddr(
        ctx.device, "vkGetPipelineExecutablePropertiesKHR");
    auto getStats = (PFN_vkGetPipelineExecutableStatisticsKHR)vkGetDeviceProcAddr(
        ctx.device, "vkGetPipelineExecutableStatisticsKHR");
    if (!getProps || !getStats) return "";
    VkPipelineInfoKHR pi{VK_STRUCTURE_TYPE_PIPELINE_INFO_KHR};
    pi.pipeline = p;
    uint32_t ne = 0;
    getProps(ctx.device, &pi, &ne, nullptr);
    std::string out;
    for (uint32_t e = 0; e < ne; ++e)
    {
        VkPipelineExecutableInfoKHR ei{VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_INFO_KHR};
        ei.pipeline = p;
        ei.executableIndex = e;
        uint32_t ns = 0;
        getStats(ctx.device, &ei, &ns, nullptr);
        std::vector<VkPipelineExecutableStatisticKHR> st(ns, {VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_STATISTIC_KHR});
        getStats(ctx.device, &ei, &ns, st.data());
        for (auto &s : st)
        {
            char buf[160];
            switch (s.format)
            {
            case VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_BOOL32_KHR: snprintf(buf, sizeof(buf), "%s %d", s.name, (int)s.value.b32); break;
            case VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_INT64_KHR: snprintf(buf, sizeof(buf), "%s %lld", s.name, (long long)s.value.i64); break;
            case VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_UINT64_KHR: snprintf(buf, sizeof(buf), "%s %llu", s.name, (unsigned long long)s.value.u64); break;
            default: snprintf(buf, sizeof(buf), "%s %.3g", s.name, s.value.f64); break;
            }
            out += std::string(out.empty() ? "" : ", ") + buf;
        }
    }
    return out;
}

// Harness `shaders reload`: rebuild the march and resolve pipelines from the SPVs now on disk, so a
// shader iteration costs a rebuild instead of an app launch (launches are what freeze this machine,
// docs/FREEZES.md). Same layouts, so descriptors and push constants carry over. Returns what failed.
// marchSpv overrides the march's file (an A/B against another build in one batch); stats receives the
// driver's statistics for the new march pipeline.
std::string SatelliteSim::reloadCloudsV2Shaders(VulkanContext &ctx, const std::string &marchSpv, std::string &stats,
                                                uint32_t wgX, uint32_t wgY)
{
    if (wgX && wgY) { cv2MarchWg[0] = wgX; cv2MarchWg[1] = wgY; }
    vkDeviceWaitIdle(ctx.device);
    std::string err;
    auto swap = [&](VkPipeline &p, const char *spv, VkPipelineLayout layout, const uint32_t *wg) {
        try
        {
            VkPipeline np = makeComputePipeline(ctx, spv, layout, wg);
            if (p) vkDestroyPipeline(ctx.device, p, nullptr);
            p = np;
        }
        catch (const std::exception &e)
        {
            err += std::string(e.what()) + "; ";
        }
    };
    swap(cv2MarchPipeline, marchSpv.empty() ? "shaders/cloud_v2_march.comp.spv" : marchSpv.c_str(), cv2MarchPipeLayout,
         cv2MarchWg);
    {
        const uint32_t specB[3] = {cv2MarchWg[0], cv2MarchWg[1], 1u};
        try
        {
            VkPipeline np = makeComputePipeline(ctx, marchSpv.empty() ? "shaders/cloud_v2_march.comp.spv" : marchSpv.c_str(),
                                                cv2MarchPipeLayout, specB, 3);
            if (cv2MarchPassBPipeline) vkDestroyPipeline(ctx.device, cv2MarchPassBPipeline, nullptr);
            cv2MarchPassBPipeline = np;
        }
        catch (const std::exception &e)
        {
            err += std::string(e.what()) + "; ";
        }
    }
    swap(cv2ResolvePipeline, "shaders/cloud_v2_resolve.comp.spv", cv2ResolvePipeLayout, nullptr);
    swap(cv2TilesPipeline, "shaders/cloud_v2_tiles.comp.spv", cv2MarchPipeLayout, nullptr);
    swap(cv2LightningPipeline, "shaders/cloud_v2_lightning.comp.spv", cv2MarchPipeLayout, nullptr);
    swap(cv2LightVolPipeline, "shaders/cloud_v2_lightvol.comp.spv", cv2MarchPipeLayout, nullptr);
    swap(cloudMarchPipeline, "shaders/cloud_march.comp.spv", cloudMarchPipeLayout, nullptr);   // the composite
    cv2HistoryValid = false;
    stats = pipelineStatsLine(ctx, cv2MarchPipeline);
    return err;
}

void SatelliteSim::destroyCloudsV2(VkDevice device)
{
    destroyCloudsV2Targets(device);
    if (cv2MarchPipeline) vkDestroyPipeline(device, cv2MarchPipeline, nullptr);
    if (cv2ResolvePipeline) vkDestroyPipeline(device, cv2ResolvePipeline, nullptr);
    if (cv2LightningPipeline) vkDestroyPipeline(device, cv2LightningPipeline, nullptr);
    if (cv2MarchPassBPipeline) vkDestroyPipeline(device, cv2MarchPassBPipeline, nullptr);
    cv2MarchPassBPipeline = VK_NULL_HANDLE;
    if (cv2TilesPipeline) vkDestroyPipeline(device, cv2TilesPipeline, nullptr);
    cv2TilesPipeline = VK_NULL_HANDLE;
    if (cv2TileBuf) vkDestroyBuffer(device, cv2TileBuf, nullptr);
    if (cv2TileMem) vkFreeMemory(device, cv2TileMem, nullptr);
    cv2TileBuf = VK_NULL_HANDLE; cv2TileMem = VK_NULL_HANDLE;
    if (cv2LightVolPipeline) vkDestroyPipeline(device, cv2LightVolPipeline, nullptr);
    cv2LightVolPipeline = VK_NULL_HANDLE;
    if (cv2LightVolView) vkDestroyImageView(device, cv2LightVolView, nullptr);
    if (cv2LightVolImg) vkDestroyImage(device, cv2LightVolImg, nullptr);
    if (cv2LightVolMem) vkFreeMemory(device, cv2LightVolMem, nullptr);
    cv2LightVolView = VK_NULL_HANDLE; cv2LightVolImg = VK_NULL_HANDLE; cv2LightVolMem = VK_NULL_HANDLE;
    for (VkImageView v : cv2WxMipViews)
        vkDestroyImageView(device, v, nullptr);
    cv2WxMipViews.clear();
    cv2WxSets.clear();
    if (cv2WxPipeline) vkDestroyPipeline(device, cv2WxPipeline, nullptr);
    if (cv2WxPipeLayout) vkDestroyPipelineLayout(device, cv2WxPipeLayout, nullptr);
    if (cv2WxPool) vkDestroyDescriptorPool(device, cv2WxPool, nullptr);
    if (cv2WxSetLayout) vkDestroyDescriptorSetLayout(device, cv2WxSetLayout, nullptr);
    cv2WxPipeline = VK_NULL_HANDLE; cv2WxPipeLayout = VK_NULL_HANDLE; cv2WxPool = VK_NULL_HANDLE;
    cv2WxSetLayout = VK_NULL_HANDLE;
    cv2LightningPipeline = VK_NULL_HANDLE;
    if (cv2FlashMem) { vkUnmapMemory(device, cv2FlashMem); cv2FlashMapped = nullptr; }
    if (cv2FlashBuf) vkDestroyBuffer(device, cv2FlashBuf, nullptr);
    if (cv2FlashMem) vkFreeMemory(device, cv2FlashMem, nullptr);
    cv2FlashBuf = VK_NULL_HANDLE; cv2FlashMem = VK_NULL_HANDLE;
    if (cv2BlueNoiseBuf) vkDestroyBuffer(device, cv2BlueNoiseBuf, nullptr);
    if (cv2BlueNoiseMem) vkFreeMemory(device, cv2BlueNoiseMem, nullptr);
    cv2BlueNoiseBuf = VK_NULL_HANDLE; cv2BlueNoiseMem = VK_NULL_HANDLE;
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
    VkDescriptorBufferInfo flashes{cv2FlashBuf, 0, VK_WHOLE_SIZE};
    // The sky set's binding 7 (sat_sky.frag's flat layers, its SKY_ENV / SKY_LITE variants and Potato):
    // the weather cube, so every flat stand-in shows the same evolving map as the volumetric clouds.
    VkDescriptorImageInfo weatherCube{cv2RepeatSampler, cv2WeatherView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet w[] = {
        imageWrite(skyDescSet, 7, TEX, &weatherCube),
        bufferWrite(cloudMarchDescSet, 21, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &flashes),
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
    for (float v : {cv2Coverage, cv2CoverClear, cv2CoverFull, cv2Density, cv2Detail, cv2Wobble, cv2Lean,
                    cv2InteriorErosion, cv2ColumnEdge, cv2AmbientGain, cv2SunGain, cv2MoonGain, cv2BounceGain,
                    cv2Powder, cv2MsExtinction, cv2MsStrength, cv2PhaseG, cv2LightLenM, cv2LightSteps,
                    cv2StepBaseM, cv2StepGrowth, cv2StepMaxM, cv2MaxIters, cv2MaxDistKm, cv2ShapePeriodM,
                    cv2DetailPeriodM, cv2CellPeriodM, cv2ClusterPeriodM, (float)cv2DebugView, cv2EdgeSharpness,
                    cv2WeatherWarpKm, cv2FullRateAboveKm, cv2MidAmount, cv2StormScale, cv2StormDetail,
                    cv2Anvil, cv2BaseRoughness, cv2HighAmount, cv2HighDensity, cv2CirrusStretch,
                    cv2RainAmount, cv2OpticsGain, cv2CirrusPeriodM, cv2MidPeriodM, cv2MidDensity,
                    cv2CloudSunRayleigh, cv2TwilightSky, cv2FlowWarp, cv2FlowPeriodKm, cv2LayerSpread,
                    cv2TopHeavy, cv2TowerTop, cv2CirrusFieldKm, cv2CirrusFlow, cv2BaseFlatness,
                    cv2CbColumns, cv2CbSpacingKm, cv2CbRadiusKm, cv2CbCumulusTopKm, cv2CbWaist, cv2CbFlare,
                    cv2CbHeadDriftKm, cv2CbLobes, cv2CbHeadLobes, cv2CbSparsity, cv2CbOvershootKm, cv2AnvilThickKm, cv2AnvilHangKm, cv2CbCumulusReachKm, cv2LightLodFootprintM, cv2LightningRate, cv2CbFill, cv2CbCumulusVar, cv2FogAmount, cv2FogDepthM, cv2FogDensity, cv2DustAmount,
                    cv2DustHeightM, cv2DustDensity, (float)(debugDisableMask & 2048u)})
        mix(v);
    for (const GpuCloudV2Type &t : cv2Types)
        for (int k = 0; k < 4; ++k)
        {
            mix(t.alt[k]);
            mix(t.shape[k]);
            mix(t.look[k]);
        }

    bool valid = cv2HistoryValid && h == cv2SettingsHash &&
                 glm::length(eye - cv2PrevEye) < 20000.0 && std::abs(simT - cv2PrevSimT) < 600.0;

    // The map's longitude drift: the same angle v1's flat layer (layer 0, driftMult 1) and the
    // ambience's cloud driver read, so the volumetric clouds sit where the map puts them. EVERY part
    // of the field is read in the drifted frame (the weather cube and the noise volumes alike), so
    // the clouds move with the map as one body, and the resolve can reproject that motion exactly:
    // a point p now shows what Rz(drift - prevDrift) p showed last frame. Until 2026-09-28 only the
    // camera was reprojected; under time warp the one-in-four refreshed pixel of each 2x2 block
    // disagreed with its three stale neighbours and far clouds broke into 2x2 squares.
    const double drift = cloudDriftPhase();
    const double cD = std::cos(drift), sD = std::sin(drift);
    auto rotD = [&](const glm::dvec3 &v, double c, double s)
    { return glm::dvec3(v.x * c - v.y * s, v.x * s + v.y * c, v.z); };
    const double dDrift = drift - cv2PrevDrift;
    const double cDD = std::cos(dDrift), sDD = std::sin(dDrift);
    const glm::dvec3 windDir = glm::normalize(glm::dvec3(0.83, 0.52, 0.19));
    const double dSimT = simT - cv2PrevSimT;

    p.prevSkyView = cv2PrevSkyView;
    p.prevObs = glm::vec4(cv2PrevObsDir, cv2PrevTanHalf);
    // The eye's displacement relative to the moving clouds. A cloud point p now showed its content
    // at Rz(dDrift) p + Rz(-prevDrift) w dt last frame (the noise is read at Rz(drift) p + w t), so
    // the resolve turns the eye-relative point by dDrift and adds this: Rz(dDrift) eye - prevEye
    // plus the wind's shift, taken at 0.9 of the shape wind (the cells, which place the clouds, move
    // at 0.85 of it, the lobes at 1.0).
    const glm::dvec3 windShift = rotD(windDir * ((double)cv2WindMps * 0.9 * dSimT),
                                      std::cos(-cv2PrevDrift), std::sin(-cv2PrevDrift));
    p.obsDelta = glm::vec4(glm::vec3(rotD(eye, cDD, sDD) - cv2PrevEye + windShift), cv2PrevAspect);
    // What is left unreprojected: the volumes' relative slide (detail 1.6, cluster 0.6 of the wind).
    p.motion = glm::vec4((float)std::abs((double)cv2WindMps * 0.7 * dSimT), std::clamp(cv2HistoryWeightMoving, 0.05f, 0.5f),
                         std::clamp(cv2CbHeadLobes, 0.0f, 1.5f), 0.0f);

    // Noise anchors: the observer's SEA-LEVEL point (what the shaders measure from), turned into
    // the drifted frame, plus a small wind, in periods, reduced in double. Each volume moves at its
    // own rate, so the shape moves through the cells and the detail through the shape: the clouds
    // evolve on top of the drift instead of sliding as one.
    const glm::dvec3 sea = rotD(up * R, cD, sD);
    auto anchor = [&](double periodM, double windMul)
    {
        const glm::dvec3 a = (sea + windDir * ((double)cv2WindMps * windMul * simT)) / periodM;
        return glm::vec4((float)fracPos(a.x), (float)fracPos(a.y), (float)fracPos(a.z), (float)(1.0 / periodM));
    };
    p.anchorShape = anchor(cv2ShapePeriodM, 1.0);
    p.anchorDetail = anchor(cv2DetailPeriodM, 1.6);
    p.anchorCluster = anchor(cv2ClusterPeriodM, 0.6);
    p.anchorCell = anchor(cv2CellPeriodM, 0.85);
    const double stormScale = std::clamp((double)cv2StormScale, 0.25, 16.0);
    p.anchorStorm = anchor(cv2ShapePeriodM * stormScale, 1.0);
    p.anchorStormDetail = anchor(cv2DetailPeriodM * stormScale, 1.6);
    p.storm = glm::vec4((float)stormScale, cv2StormDetail, cv2Anvil, cv2BaseRoughness);
    // The high layer's streak coordinates: along the wind = the drifted longitude x R, with a period
    // that divides the equator an integer number of times (no seam at the antimeridian), stretched;
    // across it = latitude x R at the shape period. The jet moves the fibres east over the map.
    {
        const double twoPiR = glm::two_pi<double>() * R;
        // Its own period (cv2CirrusPeriodM): it used to be the cumulus shape period, so enlarging the
        // cumulus lobes turned the fibres into blobs.
        const double cirrusP = std::clamp((double)cv2CirrusPeriodM, 500.0, 60000.0);
        const double along = cirrusP * std::clamp((double)cv2CirrusStretch, 1.0, 40.0);
        // A multiple of 4: the bundle read (clouds_v2.glsl) is 4x coarser and must not seam either.
        const double n = std::max(4.0, std::round(twoPiR / along / 4.0) * 4.0);
        const double pu = twoPiR / n;
        p.high = glm::vec4(cv2HighAmount, (float)(1.0 / pu), (float)fracPos(-(double)cv2CirrusWindMps * simT / pu),
                           (float)(1.0 / cirrusP));
        p.high2 = glm::vec4(cv2HighDensity,
                            (float)(1.0 / (std::clamp((double)cv2CirrusFieldKm, 50.0, 8000.0) * 1000.0)),
                            std::clamp(cv2CirrusFlow, 0.0f, 4.0f), std::clamp(cv2BaseFlatness, 0.0f, 1.0f));
    }
    p.rain = glm::vec4(cv2RainAmount, cv2OpticsGain, cv2RainStreaks, cv2WindMps); // w: the streaks' wind
    // w: a beam's intensity (sat_orbit.comp: 1361 x area x F x cos x beamGain) back to its reflecting
    // area, so the shaders can light with the physical irradiance (area / the spot's area, in Suns).
    p.beam = glm::vec4(std::max(cv2BeamShafts, 0.0f), std::max(cv2BeamHaze, 0.0f), std::max(cv2BeamLight, 0.0f),
                       beamGain > 0.0f ? 1.0f / (1361.0f * beamGain) : 0.0f);
    p.anchorMid = anchor(std::clamp((double)cv2MidPeriodM, 500.0, 60000.0), 1.0);
    p.atmo = glm::vec4(std::max(cv2CloudSunRayleigh, 0.0f), std::max(cv2TwilightSky, 0.0f),
                       std::max(cv2MidDensity, 0.0f), std::max(cv2BeamLines, 0.0f));
    {
        const double flowP = std::clamp((double)cv2FlowPeriodKm, 200.0, 20000.0) * 1000.0;
        // The analytic flow (cv2FlowDisp): x = its slow evolution phase (cycles; the waves move about
        // as fast as 0.3 x the wind over the period), w = 1 / period. Not an observer-anchored frac —
        // the field is evaluated on the drifted sphere directly.
        p.anchorFlow = glm::vec4((float)fracPos((double)cv2WindMps * 0.3 * simT / flowP), 0.0f, 0.0f,
                                 (float)(1.0 / flowP));
        p.flow = glm::vec4((float)(std::clamp((double)cv2FlowWarp, 0.0, 0.1) * flowP),
                           std::clamp(cv2LayerSpread, 0.0f, 1.0f),
                           std::clamp(cv2TopHeavy, 0.0f, 2.0f), (cv2TowerTop <= 0.0f ? 0.0f : std::clamp(cv2TowerTop, 0.3f, 0.95f)));
    }

    {
        // The Cb column lattice: 64 cells per period, so a cell's id mod 64 (its hash) is stable as the
        // observer moves; carried with the cells (0.85 of the wind), like the field they rise from.
        const double cellM = std::clamp((double)cv2CbSpacingKm, 8.0, 120.0) * 1000.0;
        p.anchorCol = anchor(cellM * 64.0, 0.85);
        p.column = glm::vec4(std::max(cv2CbColumns, 0.0f), (float)cellM,
                             std::clamp(cv2CbRadiusKm, 1.0f, 20.0f) * 1000.0f,
                             std::clamp(cv2CbCumulusTopKm, 2.0f, 12.0f) * 1000.0f);
        p.column2 = glm::vec4(std::clamp(cv2CbWaist, 0.2f, 1.5f), std::clamp(cv2CbFlare, 0.5f, 3.0f),
                              std::clamp(cv2CbHeadDriftKm, 0.0f, 30.0f) * 1000.0f, std::clamp(cv2CbLobes, 0.0f, 1.5f));
        p.anvil2 = glm::vec4(std::clamp(cv2AnvilThickKm, 0.3f, 6.0f) * 1000.0f,
                             std::clamp(cv2AnvilHangKm, 0.0f, 6.0f) * 1000.0f,
                             std::clamp(cv2CbSparsity, 0.0f, 1.0f), std::clamp(cv2CbOvershootKm, 0.0f, 3.0f) * 1000.0f);
        // The storm cumulus rises within this distance of a tower's edge (review 5: it was a weather mip,
        // the map's storm REGION, which drew a continent-wide 5-7 km cumulus floor).
        p.column3 = glm::vec4(std::clamp(cv2CbCumulusReachKm, 0.0f, 60.0f) * 1000.0f,
                              0.0f, std::clamp(cv2CbCumulusVar, 0.0f, 0.8f), std::clamp(cv2CbFill, 0.02f, 1.0f));
    }
    p.column3.y = std::max(cv2LightLodFootprintM, 0.0f);   // light LOD (cloud_v2_march.comp)
    // Lightning: its schedule runs on SIM time (deterministic, reversible), wrapped where a float still
    // resolves ~10 ms (a flash in progress at the wrap is cut; once per ~28 h of sim time).
    // The light volume: +-"God ray range" about the eye (a low Sun's shafts come from storms hundreds of
    // km away), sea level to 16 km, four of its 32 levels per frame.
    p.lightVol = glm::vec4((float)((cv2Frame % (kCv2LightVolZ / kCv2LightVolLevelsPerFrame)) * kCv2LightVolLevelsPerFrame),
                           std::clamp(cv2GodrayRangeKm, 50.0f, 1500.0f) * 1000.0f, 16000.0f, std::max(cv2Godrays, 0.0f));
    // z: the eye moved (over 1 m since the last frame): the march switches its ray jitter from IGN to
    // blue noise (cloud_v2_march.comp). It was over 150 m (fast flight) until review 7: at a walking or
    // hover speed the moving history weight (0.7, the user's 1.0) let IGN's regular structure through as
    // a dotted honeycomb over lit cloud tops (user snapshot 4).
    p.misc2 = glm::vec4(std::clamp(cv2LightningSprites, 0.0f, 1.0f), 1.0f,
                        glm::length(eye - cv2PrevEye) > 1.0 ? 1.0f : 0.0f, 0.0f);
    p.lightning = glm::vec4(std::max(cv2LightningRate, 0.0f), std::max(cv2LightningGlow, 0.0f),
                            std::max(cv2LightningBolt, 0.0f), (float)std::fmod((double)simDayJ2000 * 86400.0 + simSecInDay, 100000.0));

    static const int kOffsets[4][2] = {{0, 0}, {1, 1}, {1, 0}, {0, 1}};
    const int *o = kOffsets[cv2Frame & 3u];
    p.frame = glm::vec4((float)(cv2Frame & 0xFFFFu), valid ? 1.0f : 0.0f, (float)o[0], (float)o[1]);
    p.march = glm::vec4(cv2StepBaseM, cv2StepGrowth, cv2StepMaxM, cv2MaxDistKm * 1000.0f);
    p.light = glm::vec4(cv2LightLenM, std::round(std::clamp(cv2LightSteps, 1.0f, 12.0f)), cv2MsExtinction,
                        cv2MsStrength);
    p.look = glm::vec4(cv2Coverage, cv2Density, cv2Detail, cv2AmbientGain);
    p.look2 = glm::vec4(cv2BounceGain, cv2SunGain, cv2Powder, cv2HistoryWeight);
    p.phase = glm::vec4(cv2PhaseG, 0.6f, -0.25f, 0.4f);

    float lo = 1e9f, hi = 0.0f;
    for (int i = 0; i < kCloudV2Types; ++i)
    {
        p.types[i] = cv2Types[i];
        lo = std::min(lo, cv2Types[i].alt.x - cv2Types[i].alt.w);
        hi = std::max(hi, cv2Types[i].alt.x + (cv2Types[i].alt.y - cv2Types[i].alt.x) * 1.35f);
    }
    // Rain falls to the ground: with it on, the shell reaches sea level.
    // Fog and dust (cv2FogDust); knockout bit 2048 (the old "fog layer" bit, which the Low / Planetarium /
    // Potato presets set) switches them off.
    const bool fogOff = (debugDisableMask & 2048u) != 0u;
    p.fog  = glm::vec4(fogOff ? 0.0f : std::max(cv2FogAmount, 0.0f), std::max(cv2FogDepthM, 20.0f),
                       std::max(cv2FogDensity, 0.0f), fogOff ? 0.0f : std::max(cv2DustAmount, 0.0f));
    p.fog2 = glm::vec4(std::max(cv2DustHeightM, 100.0f), std::max(cv2DustDensity, 0.0f), cv2DebugView == 11 ? 1.0f : 0.0f,
                       fogOff ? 0.0f : std::max(cv2IceFogAmount, 0.0f));
    {   // The Sun, Earth-fixed (the sim's rotation angle kOmegaEarth t, as the weather bake uses).
        const double g = std::fmod(satphot::kOmegaEarth * simT, 6.283185307179586);
        const glm::dvec3 si = glm::dvec3(sunDirECI);
        p.sunE = glm::vec4(glm::vec3(glm::normalize(glm::dvec3(si.x * std::cos(g) + si.y * std::sin(g),
                                                               -si.x * std::sin(g) + si.y * std::cos(g), si.z))),
                           std::max(cv2IceFogDensity, 0.0f));   // w: the ice fog's extinction
    }
    {   // Rain / snow at the eye: a local frame fixed to the nearest 0.25-degree point (so the drops are
        // world-fixed: walking moves through them), the eye's position in it in double, wrapped to 1024 m
        // (every lattice cell divides it); and the air temperature at the eye — sea-level climate by
        // latitude (-18 + 45 cos^1.5, ~27 C at the equator, ~9 at 45, ~-12 at 75), a season swing of
        // 0.25 x |lat| (at most 15 C) following the Sun's declination in that hemisphere, 6.5 C per km of altitude.
        const glm::dvec3 up = glm::normalize(glm::dvec3(obsDir));
        const double lat = std::asin(std::clamp(up.z, -1.0, 1.0)), lon = std::atan2(up.y, up.x);
        const double q = glm::radians(0.25);
        const double latR = std::round(lat / q) * q, lonR = std::round(lon / q) * q;
        const glm::dvec3 U0(std::cos(latR) * std::cos(lonR), std::cos(latR) * std::sin(lonR), std::sin(latR));
        const glm::dvec3 E0 = glm::normalize(glm::cross(glm::dvec3(0.0, 0.0, 1.0), U0));
        const glm::dvec3 N0 = glm::cross(U0, E0);
        const float ground = (terrainFrameMapped && (debugDisableMask & 1024u) == 0) ? terrainFrameMapped[1] : obsTerrainH;
        const double eyeAsl = std::max((double)ground + 2.0, (double)obsHeightOffset);
        const glm::dvec3 eye = up * (6371000.0 + eyeAsl);
        auto wrap = [](double v) { v = std::fmod(v, 1024.0); return (float)(v < 0.0 ? v + 1024.0 : v); };
        p.rainE = glm::vec4(glm::vec3(E0), wrap(glm::dot(eye, E0)));
        p.rainN = glm::vec4(glm::vec3(N0), wrap(glm::dot(eye, N0)));
        p.rainU = glm::vec4(glm::vec3(U0), wrap(glm::dot(eye, U0)));
        const double latD = glm::degrees(lat);
        const double sunDec = std::asin(std::clamp((double)glm::normalize(sunDirECI).z, -1.0, 1.0));
        const double season = (sunDec / glm::radians(23.44)) * (latD >= 0.0 ? 1.0 : -1.0);   // +1 local midsummer
        // The season swing is 0.25 x |lat|, at most 15 C: 0.35 x |lat| (+-27 C at 77 deg) made the
        // Antarctic coast +13 C in its summer, rain under snow clouds and a rainbow (review 6).
        const double tC = -18.0 + 45.0 * std::pow(std::cos(lat), 1.5) + std::min(0.25 * std::abs(latD), 15.0) * season
                        - 0.0065 * eyeAsl;
        // y: sim time wrapped to 600 s in double (the drops' fall; a float time of day steps every 8 ms).
        // z "Snow wind" (x the flakes' drift: blizzards), w "Drop distance" (m: the lattice's last layer).
        p.precip = glm::vec4((float)tC, (float)std::fmod(simSecInDay, 600.0), std::clamp(cv2SnowWind, 0.0f, 8.0f),
                             std::clamp(cv2DropDistM, 8.0f, 512.0f));
        cv2EyeTempC = (float)tC;
    }
    const bool lowFloor = cv2RainAmount > 0.0f || p.fog.x > 0.0f || p.fog.w > 0.0f || p.fog2.w > 0.0f;   // they reach the ground
    p.shell = glm::vec4(lowFloor ? 0.0f : std::max(lo, 0.0f), hi, cv2DebugView == 11 ? 0.0f : (float)cv2DebugView,
                        cv2DetailLodStartM);
    // zw: cos/sin of the drift since last frame (the resolve turns a cloud point by it).
    p.extra = glm::vec4(cv2EdgeSharpness, cv2WeatherWarpKm * 1000.0f / (float)R, (float)cDD, (float)sDD);
    p.cover = glm::vec4(cv2CoverClear, std::max(cv2CoverFull, cv2CoverClear + 0.01f), (float)cD, (float)sD);
    p.form = glm::vec4(cv2Wobble, cv2Lean, cv2InteriorErosion, std::max(cv2ColumnEdge, 0.5f));
    // Full rate: from altitude every half-res pixel is marched every frame. The sparse 1-in-4
    // march relies on history, which a moving orbital camera keeps invalidating; the fallback was a
    // 1/8-resolution upsample (the "pixelated mess" from space). Up there a ray crosses only a thin
    // shell, so marching all of them is cheap.
    // ...but only while the view MOVES (pass 19): a still view's sparse march converges on the same
    // image through the history, at ~1/3 of the cost (storm views: 14 FPS overlooking anvils). Any
    // camera turn past a third of a half-res pixel, eye motion, zoom, time warp or history loss
    // switches back at once; sparse resumes after 8 still frames.
    {
        float dRot = 0.0f;
        for (int c = 0; c < 3; ++c)
            for (int r = 0; r < 3; ++r)
                dRot = std::max(dRot, std::abs(cpc.skyView[c][r] - cv2PrevSkyView[c][r]));
        const float pixAng = 2.0f * tanHalf / (float)std::max(cv2HalfH, 1u);
        const bool moving = !valid || dRot > 0.3f * pixAng || glm::length(eye - cv2PrevEye) > 1.0 ||
                            std::abs(simT - cv2PrevSimT) > 0.5 || std::abs(tanHalf - cv2PrevTanHalf) > 1e-5f;
        cv2StillFrames = moving ? 0 : std::min(cv2StillFrames + 1, 1000);
    }
    cv2FullRateNow = eyeH > (double)cv2FullRateAboveKm * 1000.0 &&
                     !(cv2SparseWhenStill > 0.5f && cv2StillFrames >= 8);
    // "Half rate while moving": where it would run at full rate the march takes a checkerboard of the
    // half-res pixels, alternating each frame (every stale pixel's four neighbours are fresh: the
    // resolve's estimate for it), at about half the cost.
    cv2HalfRateNow = cv2FullRateNow && cv2HalfRateMoving > 0.5f;
    // The adaptive rate ("dynamic sampling"): where the march would run at full rate because the view
    // moves, it runs the sparse grid (pass A) and only the tiles whose clouds show parallax, or whose
    // history comes from off screen, at full rate (pass B). A pan reprojects exactly: only the uncovered
    // edge goes full. Not with no history or in fast flight (> 150 m a frame: everything has parallax).
    {
        const uint32_t tiles = ((cv2HalfW + 31) / 32) * ((cv2HalfH + 31) / 32);
        cv2AdaptiveNow = cv2FullRateNow && !cv2HalfRateNow && cv2AdaptiveRate > 0.5f && valid &&
                         glm::length(eye - cv2PrevEye) <= 150.0 && cv2MarchWg[0] == 16 && cv2MarchWg[1] == 16 &&
                         tiles <= kCv2MaxTiles && cv2MarchPassBPipeline != VK_NULL_HANDLE && cv2TilesPipeline != VK_NULL_HANDLE;
        if (cv2AdaptiveNow)
            cv2FullRateNow = false;
    }
    p.misc2.w = std::max(cv2AdaptiveParallaxPx, 0.05f);
    p.misc = glm::vec4(std::round(std::clamp(cv2MaxIters, 32.0f, 1024.0f)), cv2MoonGain,
                       cv2AdaptiveNow ? 3.0f : cv2HalfRateNow ? 2.0f : (cv2FullRateNow ? 1.0f : 0.0f), cv2MidAmount);
    std::memcpy(cv2ParamsMapped, &p, sizeof(p));

    // This frame becomes the next frame's "previous".
    {
        cv2PrevSkyView = cpc.skyView;
        cv2PrevObsDir = glm::vec3(up);
        cv2PrevEye = eye;
        cv2PrevTanHalf = tanHalf;
        cv2PrevAspect = cpc.aspect;
        cv2PrevSimT = simT;
        cv2PrevDrift = drift;
        cv2SettingsHash = h;
        cv2HistoryValid = true;
        ++cv2Frame;
    }
    (void)ctx;
}

// The weather bake's push constants for one face (-1: all six) at one mip, at the current sim time.
// Two copies of the map advected over windows of P, offset by P/2, each weighted sin^2 of its own
// phase (0 at its reset, the two summing to 1): the flow-map double-phase trick (decision A).
SatelliteSim::GpuWeatherPC SatelliteSim::weatherEvoPC(int face, uint32_t mip) const
{
    GpuWeatherPC pc{};
    pc.faceSize = (int32_t)(kCv2WeatherFace >> mip);
    pc.srcLod = 1.0f + (float)mip;   // the 8192-wide source's texels are ~half a face texel's at mip 0
    pc.face = face;
    const double t = (double)simDayJ2000 * 86400.0 + simSecInDay;
    const double P = std::max((double)cv2EvoWindowH, 0.1) * 3600.0;
    const double ph0 = t / P - std::floor(t / P), ph1 = ph0 + 0.5 - std::floor(ph0 + 0.5);
    const float w0 = (float)(std::sin(3.14159265358979 * ph0) * std::sin(3.14159265358979 * ph0));
    if (cv2EvoWindMps > 0.0f)
        pc.evo0 = glm::vec4((float)(ph0 * P), (float)(ph1 * P), w0, 1.0f - w0);
    else
        pc.evo0 = glm::vec4(0.0f, 0.0f, 1.0f, 0.0f);   // the static map
    pc.evo1 = glm::vec4(std::max(cv2EvoWindMps, 0.0f), std::max(cv2EvoGrowth, 0.0f),
                        (float)std::fmod(t, 1.0e6), 0.0f);
    // The Sun three hours ago, Earth-fixed (the sim's rotation angle, no GMST offset: kOmegaEarth t;
    // 3 h back it stood 45 deg further east), then into the map's (drifted) frame as cv2Drift turns it.
    const double g = std::fmod(satphot::kOmegaEarth * t, 6.283185307179586) - 3.0 * 3600.0 * satphot::kOmegaEarth;
    const glm::dvec3 si = glm::dvec3(sunDirECI);
    const glm::dvec3 se(si.x * std::cos(g) + si.y * std::sin(g), -si.x * std::sin(g) + si.y * std::cos(g), si.z);
    const double dr = cloudDriftPhase();
    const glm::dvec3 sm(se.x * std::cos(dr) - se.y * std::sin(dr), se.x * std::sin(dr) + se.y * std::cos(dr), se.z);
    pc.evo2 = glm::vec4(glm::vec3(glm::normalize(sm)), std::max(cv2EvoDiurnal, 0.0f));
    return pc;
}

// One face of the weather cube per frame, all its mips, re-baked when sim time has moved on enough to
// show (the evolution at 1x is metres per frame; in time warp every frame) or the settings changed.
// The face is taken out of SHADER_READ_ONLY for the writes and returned before the passes read it.
void SatelliteSim::recordWeatherEvolution(VkCommandBuffer cmd)
{
    if (!cv2WxPipeline || cv2WxSets.size() != kCv2WeatherMips)
        return;
    const double t = (double)simDayJ2000 * 86400.0 + simSecInDay;
    const uint64_t h = std::hash<float>{}(cv2EvoWindMps) ^ (std::hash<float>{}(cv2EvoGrowth) * 31u)
                     ^ (std::hash<float>{}(cv2EvoWindowH) * 131u) ^ (std::hash<float>{}(cv2EvoDiurnal) * 1031u);
    const bool evolving = cv2EvoWindMps > 0.0f || cv2EvoGrowth > 0.0f || cv2EvoDiurnal > 0.0f;
    if (h != cv2WxHash)
    {
        for (double &b : cv2WxBakedT)
            b = -1e30;   // every face stale: re-bake with the new settings (off: back to the static map)
        cv2WxHash = h;
    }
    // A face is due once it is this much sim time behind (a few km of wind drift at most).
    const double due = evolving ? std::clamp(300.0 / std::max((double)cv2EvoWindMps, 1.0), 5.0, 120.0) : 1e30;
    int face = -1;
    double worst = 0.0;
    for (int f = 0; f < 6; ++f)
    {
        const double lag = std::fabs(t - cv2WxBakedT[f]);
        if ((cv2WxBakedT[f] < -1e29 || lag > due) && lag > worst)
        {
            worst = lag;
            face = f;
        }
    }
    if (face < 0)
        return;
    cv2WxBakedT[face] = t;
    ++cv2WxRebakes;

    auto faceBarrier = [&](VkImageLayout from, VkImageLayout to, VkAccessFlags sa, VkAccessFlags da) {
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        b.oldLayout = from;
        b.newLayout = to;
        b.srcAccessMask = sa;
        b.dstAccessMask = da;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = cv2WeatherImg;
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, kCv2WeatherMips, (uint32_t)face, 1};
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0,
                             0, nullptr, 0, nullptr, 1, &b);
    };
    faceBarrier(VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_SHADER_READ_BIT,
                VK_ACCESS_SHADER_WRITE_BIT);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, cv2WxPipeline);
    for (uint32_t m = 0; m < kCv2WeatherMips; ++m)
    {
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, cv2WxPipeLayout, 0, 1, &cv2WxSets[m], 0, nullptr);
        GpuWeatherPC pcv = weatherEvoPC(face, m);
        vkCmdPushConstants(cmd, cv2WxPipeLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pcv), &pcv);
        uint32_t g = std::max(1u, ((kCv2WeatherFace >> m) + 7) / 8);
        vkCmdDispatch(cmd, g, g, 1);
    }
    faceBarrier(VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_SHADER_WRITE_BIT,
                VK_ACCESS_SHADER_READ_BIT);
}

void SatelliteSim::recordCloudsV2(VkCommandBuffer cmd, VulkanContext &ctx, const CloudMarchPC &cpc)
{
    fillCloudsV2Params(ctx, cpc);
    recordWeatherEvolution(cmd);
    if (!cv2MarchPipeline)
        return;

    // scene_depth.comp wrote terrainFrameBuf (the eye height) this frame.
    memoryBarrier(cmd, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                  VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);

    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, cv2MarchPipeLayout, 0, 1, &cv2MarchDescSet, 0, nullptr);
    vkCmdPushConstants(cmd, cv2MarchPipeLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(cpc), &cpc);
    // This frame's lightning flashes (one workgroup): cloud_march.comp draws them after the resolve.
    if (cv2LightningPipeline)
    {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, cv2LightningPipeline);
        vkCmdDispatch(cmd, 1, 1, 1);
    }
    // Four levels of the light volume (the godrays read it in the march, after this barrier).
    if (cv2LightVolPipeline && cv2Godrays > 0.0f)
    {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, cv2LightVolPipeline);
        vkCmdDispatch(cmd, kCv2LightVolXY / 8, kCv2LightVolXY / 8, kCv2LightVolLevelsPerFrame / 4);
        memoryBarrier(cmd, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                      VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    }
    if (cv2AdaptiveNow)
    {
        // Pass A lists its full-rate tiles: clear the flags and the indirect args {pad, 0, 1, 1}.
        vkCmdFillBuffer(cmd, cv2TileBuf, 16, 8 * kCv2MaxTiles, 0);
        const uint32_t hdr[4] = {0u, 0u, 1u, 1u};
        vkCmdUpdateBuffer(cmd, cv2TileBuf, 0, sizeof(hdr), hdr);
        memoryBarrier(cmd, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
                      VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
        // The tile classification: one 16x16 workgroup per 32x32 tile (the sparse grid's dispatch).
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, cv2TilesPipeline);
        vkCmdDispatch(cmd, (cv2QuarterW + 15) / 16, (cv2QuarterH + 15) / 16, 1);
        memoryBarrier(cmd, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
                      VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    }
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, cv2MarchPipeline);
    const uint32_t gw = cv2HalfRateNow ? (cv2HalfW + 1) / 2 : cv2FullRateNow ? cv2HalfW : cv2QuarterW;
    const uint32_t gh = cv2FullRateNow ? cv2HalfH : cv2QuarterH;
    vkCmdDispatch(cmd, (gw + cv2MarchWg[0] - 1) / cv2MarchWg[0], (gh + cv2MarchWg[1] - 1) / cv2MarchWg[1], 1);
    if (cv2AdaptiveNow)
    {
        // Pass B: 4 workgroups (16x16) per listed 32x32 tile, dispatched from the args pass A counted.
        memoryBarrier(cmd, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_INDIRECT_COMMAND_READ_BIT,
                      VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                      VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, cv2MarchPassBPipeline);
        vkCmdDispatchIndirect(cmd, cv2TileBuf, 4);
    }

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
