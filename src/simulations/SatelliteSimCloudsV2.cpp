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

#include "../FrameProfile.h"
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
    if (ctx.pipelineStatsSupported && ctx.capturePipelineStats)
        ci.flags |= VK_PIPELINE_CREATE_CAPTURE_STATISTICS_BIT_KHR;
    VkPipeline p = VK_NULL_HANDLE;
    if (vkCreateComputePipelines(ctx.device, ctx.pipelineCache, 1, &ci, nullptr, &p) != VK_SUCCESS)
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
    createStormBuffer(ctx);   // the storm objects' towers (SatelliteSimWeather.cpp): march set binding 26
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

    createCloudMorph(ctx);
    createCloudMie(ctx);

    // ── Noise bakes (cloud_v2_noise.comp, modes 0/1/2) ──
    {
        // 0 shape, 1 detail, 2 meso, 3 the shape with cap-profile lobes (2026-10-22: same seed as 0)
        VkImage *imgs[4] = {&cv2ShapeImg, &cv2DetailImg, &cv2MesoImg, &cv2ShapeCapsImg};
        VkDeviceMemory *mems[4] = {&cv2ShapeMem, &cv2DetailMem, &cv2MesoMem, &cv2ShapeCapsMem};
        VkImageView *views[4] = {&cv2ShapeView, &cv2DetailView, &cv2MesoView, &cv2ShapeCapsView};
        for (int i = 0; i < 4; ++i)
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

        VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 4};
        VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        pi.maxSets = 4;
        pi.poolSizeCount = 1;
        pi.pPoolSizes = &ps;
        VkDescriptorPool pool = VK_NULL_HANDLE;
        vkCreateDescriptorPool(dev, &pi, nullptr, &pool);
        VkDescriptorSet sets[4];
        VkDescriptorSetLayout lays[4] = {bl, bl, bl, bl};
        VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        ai.descriptorPool = pool;
        ai.descriptorSetCount = 4;
        ai.pSetLayouts = lays;
        vkAllocateDescriptorSets(dev, &ai, sets);

        VkImageView bakeViews[4] = {};   // mip 0 only: a storage view must name one level
        VkCommandBuffer cmd = ctx.beginOneTimeCommands();
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
        for (int i = 0; i < 4; ++i)
        {
            ctx.imageBarrier(cmd, *imgs[i], 0, VK_ACCESS_SHADER_WRITE_BIT,
                             VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                             VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
            bakeViews[i] = makeView(dev, *imgs[i], VK_IMAGE_VIEW_TYPE_3D, VK_FORMAT_R8G8B8A8_UNORM, 0, 1, 1);
            VkDescriptorImageInfo ii{VK_NULL_HANDLE, bakeViews[i], VK_IMAGE_LAYOUT_GENERAL};
            VkWriteDescriptorSet w = imageWrite(sets[i], 0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &ii);
            vkUpdateDescriptorSets(dev, 1, &w, 0, nullptr);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 1, &sets[i], 0, nullptr);
            int32_t pcv[2] = {i, i == 3 ? 17 : 17 + i * 31};   // the cap shape: the shape's seed (lobes in the same places)
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
                                                       VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                                       VK_DESCRIPTOR_TYPE_STORAGE_BUFFER});   // 4: the storms
        VkPushConstantRange pcr{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(GpuWeatherPC)};
        VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pli.setLayoutCount = 1;
        pli.pSetLayouts = &bl;
        pli.pushConstantRangeCount = 1;
        pli.pPushConstantRanges = &pcr;
        VkPipelineLayout pl = VK_NULL_HANDLE;
        vkCreatePipelineLayout(dev, &pli, nullptr, &pl);
        VkPipeline pipe = makeComputePipeline(ctx, "shaders/cloud_v2_weather.comp.spv", pl);

        VkDescriptorPoolSize ps[3] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 3 * kCv2WeatherMips},
                                      {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, kCv2WeatherMips},
                                      {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, kCv2WeatherMips}};
        VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        pi.maxSets = kCv2WeatherMips;
        pi.poolSizeCount = 3;
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
            VkDescriptorBufferInfo stormInfo{stormBuf, 0, VK_WHOLE_SIZE};   // created at the top of createCloudsV2
            VkWriteDescriptorSet w[5] = {
                imageWrite(set, 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &cloudsInfo),
                imageWrite(set, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &specInfo),
                imageWrite(set, 2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &outInfo),
                imageWrite(set, 3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &elevInfo),
                bufferWrite(set, 4, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &stormInfo)};
            vkUpdateDescriptorSets(dev, 5, w, 0, nullptr);
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
        cv2WxHash = weatherBakeHash();
    }

    // ── UBO ──
    ctx.createBuffer(sizeof(GpuCloudV2Params), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                     cv2ParamsBuf, cv2ParamsMem);
    vkMapMemory(dev, cv2ParamsMem, 0, sizeof(GpuCloudV2Params), 0, &cv2ParamsMapped);
    std::memset(cv2ParamsMapped, 0, sizeof(GpuCloudV2Params));
    // The flash buffer holds the Sun shadow lattices (fine grid, cascade, both shaft rings: ~5.5 MB) that every lit
    // cloud sample and every sky pixel's god rays read. As plain HOST_VISIBLE memory it is the first such type, which on
    // NVIDIA is system RAM: each of those reads crossed PCIe behind the L2. Device-local + host-visible (the BAR heap, or
    // all of VRAM with Resizable BAR) keeps the mapping. SATLIGHTSIM_FLASH_HOSTMEM=1 forces the old placement (A/B).
    {
        const char *ev = std::getenv("SATLIGHTSIM_FLASH_HOSTMEM");
        bool done = false;
        if (!(ev && ev[0] == '1'))
        {
            try
            {
                ctx.createBuffer(kCv2FlashBufBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                 VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT
                                     | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                                 cv2FlashBuf, cv2FlashMem);
                done = true;
            }
            catch (const std::exception &)
            {
                if (cv2FlashBuf) vkDestroyBuffer(dev, cv2FlashBuf, nullptr);
                cv2FlashBuf = VK_NULL_HANDLE;
                cv2FlashMem = VK_NULL_HANDLE;
            }
        }
        if (!done)
            ctx.createBuffer(kCv2FlashBufBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                             VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                             cv2FlashBuf, cv2FlashMem);
        Log::line(std::string("clouds v2: flash buffer in ") + (done ? "device-local host-visible memory" : "host memory"));
    }
    vkMapMemory(dev, cv2FlashMem, 0, kCv2FlashBufBytes, 0, &cv2FlashMapped);
    std::memset(cv2FlashMapped, 0, kCv2FlashBufBytes);
    ctx.createBuffer(kCv2CdcHeadBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                     VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, cv2CdcBuf, cv2CdcMem);   // grown on first use
    cv2CdcBufFull = false;
    {   // Stamp 0 everywhere: every cell reads unknown until baked.
        VkCommandBuffer cmd = ctx.beginOneTimeCommands();
        vkCmdFillBuffer(cmd, cv2CdcBuf, 0, VK_WHOLE_SIZE, 0);
        ctx.endOneTimeCommands(cmd);
    }
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
        // The Sun / sky colour table (cloud_v2_sunlut.comp): layer 0 the Sun's colour, 1 the zenith sky's.
        ci.imageType = VK_IMAGE_TYPE_2D;
        ci.format = VK_FORMAT_R16G16B16A16_SFLOAT;
        ci.extent = {kCv2SunLutW, kCv2SunLutH, 1};
        ci.mipLevels = 1;
        ci.arrayLayers = 2;
        ci.samples = VK_SAMPLE_COUNT_1_BIT;
        ci.tiling = VK_IMAGE_TILING_OPTIMAL;
        ci.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (vkCreateImage(dev, &ci, nullptr, &cv2SunLutImg) != VK_SUCCESS)
            throw std::runtime_error("clouds v2: sun table vkCreateImage failed");
        VkMemoryRequirements req;
        vkGetImageMemoryRequirements(dev, cv2SunLutImg, &req);
        VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        mai.allocationSize = req.size;
        mai.memoryTypeIndex = ctx.findMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        vkAllocateMemory(dev, &mai, nullptr, &cv2SunLutMem);
        vkBindImageMemory(dev, cv2SunLutImg, cv2SunLutMem, 0);
        cv2SunLutView = makeView(dev, cv2SunLutImg, VK_IMAGE_VIEW_TYPE_2D_ARRAY, VK_FORMAT_R16G16B16A16_SFLOAT, 0, 1, 2);
        // GENERAL for good (written by the bake, sampled by the march); cleared, and baked on the first frame
        // the clouds are recorded (cv2SunLutKey).
        VkCommandBuffer cmd = ctx.beginOneTimeCommands();
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = cv2SunLutImg;
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 2};
        b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr,
                             0, nullptr, 1, &b);
        VkClearColorValue one{};
        vkCmdClearColorImage(cmd, cv2SunLutImg, VK_IMAGE_LAYOUT_GENERAL, &one, 1, &b.subresourceRange);
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
    // 12 the lightning flash list (cloud_v2_lightning.comp, same set), 13 the Sun / sky colour table (storage:
    // the bake, cloud_v2_sunlut.comp), 14 the same table (sampled: the march), 15 the blue-noise
    // tile (the ray jitter in fast flight); the adaptive rate: 16 last frame's resolved depth, 17/18 pass B's full-rate
    // targets, 19 the tile list
    // 20 the cloud morphology texture (review 17), 21 the far cloud layer (review 18, cloud_v2_far.comp)
    // 22 unused (was the layer shadow map, deleted 2026-10-22; bound to the blue-noise tile)
    // 23 the cloud density clipmap (2026-10-21, EXPERIMENT, cv2_cdc.glsl; cloud_v2_cdc.comp writes it)
    // 24 the droplets' Mie phase table (2026-10-22)
    // 25 the elevation map (2026-10-22: the terrain Sun-horizon map's bake, cloud_v2_terrsun.comp, alone reads it)
    // 26 the storm objects' towers + their bins (cv2_storms.glsl, SatelliteSimWeather.cpp)
    cv2MarchDescLayout = makeSetLayout(dev, {UBO, UBO, TEX, TEX, TEX, TEX, TEX, TEX, SSBO, SSBO, IMG, IMG, SSBO, IMG, TEX, SSBO,
                                             TEX, IMG, IMG, SSBO, TEX, IMG, SSBO, SSBO, TEX, TEX, SSBO});
    // cloud_v2_resolve.comp: 0 CloudV2Params, 1 new color, 2 new depth, 3 history, 4 out color, 5 out depth,
    // 6/7 the full-rate tiles' color/depth, 8 the tile list
    // 9 / 10 the sample ages (out, history)
    cv2ResolveDescLayout = makeSetLayout(dev, {UBO, TEX, TEX, TEX, IMG, IMG, TEX, TEX, SSBO, IMG, TEX});
    {
        VkDescriptorPoolSize ps[4] = {{UBO, 3}, {TEX, 17}, {SSBO, 9}, {IMG, 9}};
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
    cv2SunLutPipeline = makeComputePipeline(ctx, "shaders/cloud_v2_sunlut.comp.spv", cv2MarchPipeLayout);
    cv2FroxPipeline = makeComputePipeline(ctx, "shaders/cloud_v2_froxel.comp.spv", cv2MarchPipeLayout);
    cv2TshPipeline = makeComputePipeline(ctx, "shaders/cloud_v2_terrsun.comp.spv", cv2MarchPipeLayout);
    cv2FarPipeline = makeComputePipeline(ctx, "shaders/cloud_v2_far.comp.spv", cv2MarchPipeLayout);
    {   // The clipmap's two passes: specialization constant 2 = 0 check, 1 bake.
        const uint32_t spec0[3] = {0u, 0u, 0u}, spec1[3] = {0u, 0u, 1u};
        cv2CdcPipeline = makeComputePipeline(ctx, "shaders/cloud_v2_cdc.comp.spv", cv2MarchPipeLayout, spec0, 3);
        cv2CdcBakePipeline = makeComputePipeline(ctx, "shaders/cloud_v2_cdc.comp.spv", cv2MarchPipeLayout, spec1, 3);
    }

    // Static descriptors of the march set (the screen-sized ones are written by the targets).
    {
        VkDescriptorBufferInfo cpInfo{cloudParamsBuf, 0, sizeof(GpuCloudParams)};
        VkDescriptorBufferInfo v2Info{cv2ParamsBuf, 0, sizeof(GpuCloudV2Params)};
        VkDescriptorImageInfo weather{cv2RepeatSampler, cv2WeatherView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkDescriptorImageInfo shape{cv2RepeatSampler, cv2ShapeActiveView(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkDescriptorImageInfo detail{cv2RepeatSampler, cv2DetailView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkDescriptorImageInfo meso{cv2RepeatSampler, cv2MesoView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkDescriptorImageInfo morph{cv2RepeatSampler, cv2MorphView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkDescriptorImageInfo night{earthNightSampler ? earthNightSampler : noiseSampler,
                                    earthNightView ? earthNightView : noiseTexView,
                                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkDescriptorBufferInfo terr{terrainFrameBuf, 0, VK_WHOLE_SIZE};
        VkDescriptorBufferInfo beams{beamCloudLightBuf, 0, VK_WHOLE_SIZE};
        VkDescriptorBufferInfo flashes{cv2FlashBuf, 0, VK_WHOLE_SIZE};
        VkDescriptorBufferInfo blueNoise{cv2BlueNoiseBuf, 0, VK_WHOLE_SIZE};
        VkDescriptorBufferInfo tiles{cv2TileBuf, 0, VK_WHOLE_SIZE};
        VkDescriptorBufferInfo cdc{cv2CdcBuf, 0, VK_WHOLE_SIZE};
        VkDescriptorBufferInfo storms{stormBuf, 0, VK_WHOLE_SIZE};
        VkDescriptorImageInfo mieI{cv2ClampSampler, cv2MieView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkDescriptorImageInfo lvOut{VK_NULL_HANDLE, cv2SunLutView, VK_IMAGE_LAYOUT_GENERAL};
        VkDescriptorImageInfo lvIn{cv2ClampSampler, cv2SunLutView, VK_IMAGE_LAYOUT_GENERAL};
        VkDescriptorImageInfo elevI{earthElevSampler ? earthElevSampler : noiseSampler,
                                    earthElevView ? earthElevView : noiseTexView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet w[] = {
            imageWrite(cv2MarchDescSet, 25, TEX, &elevI),
            bufferWrite(cv2MarchDescSet, 15, SSBO, &blueNoise),
            bufferWrite(cv2MarchDescSet, 19, SSBO, &tiles),
            bufferWrite(cv2MarchDescSet, 22, SSBO, &blueNoise),
            bufferWrite(cv2MarchDescSet, 23, SSBO, &cdc),
            bufferWrite(cv2MarchDescSet, 26, SSBO, &storms),
            imageWrite(cv2MarchDescSet, 24, TEX, &mieI),
            bufferWrite(cv2ResolveDescSet, 8, SSBO, &tiles),
            imageWrite(cv2MarchDescSet, 13, IMG, &lvOut),
            imageWrite(cv2MarchDescSet, 14, TEX, &lvIn),
            bufferWrite(cv2MarchDescSet, 0, UBO, &cpInfo),
            bufferWrite(cv2MarchDescSet, 1, UBO, &v2Info),
            imageWrite(cv2MarchDescSet, 2, TEX, &weather),
            imageWrite(cv2MarchDescSet, 3, TEX, &shape),
            imageWrite(cv2MarchDescSet, 4, TEX, &detail),
            imageWrite(cv2MarchDescSet, 5, TEX, &meso),
            imageWrite(cv2MarchDescSet, 20, TEX, &morph),
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
    const VkExtent2D halfE = computeCloudExtent(ctx);   // the cloud grid (S9: full res with "Full-resolution clouds")
    cv2HalfW = halfE.width;
    cv2HalfH = halfE.height;
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
    make(cv2HalfW, cv2HalfH, VK_FORMAT_R32G32_SFLOAT, VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
         cv2ResolvedDepthImg, cv2ResolvedDepthMem, cv2ResolvedDepthView);
    make(cv2HalfW, cv2HalfH, VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
         cv2HistoryImg, cv2HistoryMem, cv2HistoryView);
    // Pass B's targets (the adaptive rate's full-rate tiles): 1x1 with full-res clouds, which always march sparse.
    const uint32_t fbW = cv2FullResActive() ? 1u : cv2HalfW, fbH = cv2FullResActive() ? 1u : cv2HalfH;
    make(fbW, fbH, VK_FORMAT_R16G16B16A16_SFLOAT, 0, cv2FullImg, cv2FullMem, cv2FullView);
    make(fbW, fbH, VK_FORMAT_R32G32_SFLOAT, 0, cv2FullDepthImg, cv2FullDepthMem, cv2FullDepthView);
    make(ctx.swapExtent.width, ctx.swapExtent.height, VK_FORMAT_R16G16B16A16_SFLOAT, 0, cv2FarImg, cv2FarMem, cv2FarView);
    make(cv2HalfW, cv2HalfH, VK_FORMAT_R32_SFLOAT, VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
         cv2AgeImg, cv2AgeMem, cv2AgeView);
    make(cv2HalfW, cv2HalfH, VK_FORMAT_R32_SFLOAT, VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
         cv2AgeHistImg, cv2AgeHistMem, cv2AgeHistView);

    VkCommandBuffer cmd = ctx.beginOneTimeCommands();
    for (VkImage img : {cv2NewImg, cv2NewDepthImg, cv2ResolvedImg, cv2ResolvedDepthImg, cv2HistoryImg, cv2FullImg, cv2FullDepthImg,
                        cv2FarImg, cv2AgeImg, cv2AgeHistImg})
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
    // The ages start settled: a history carried across a resize (blitted) is as old as it was.
    VkClearColorValue old{};
    old.float32[0] = 64.0f;
    vkCmdClearColorImage(cmd, cv2AgeImg, VK_IMAGE_LAYOUT_GENERAL, &old, 1, &rng);
    vkCmdClearColorImage(cmd, cv2AgeHistImg, VK_IMAGE_LAYOUT_GENERAL, &old, 1, &rng);
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
    VkDescriptorImageInfo farOut{VK_NULL_HANDLE, cv2FarView, VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorImageInfo ageOut{VK_NULL_HANDLE, cv2AgeView, VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorImageInfo ageIn{cv2ClampSampler, cv2AgeHistView, VK_IMAGE_LAYOUT_GENERAL};
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
        imageWrite(cv2ResolveDescSet, 7, TEX, &fullDepthIn),
        imageWrite(cv2MarchDescSet, 21, IMG, &farOut),
        imageWrite(cv2ResolveDescSet, 9, IMG, &ageOut),
        imageWrite(cv2ResolveDescSet, 10, TEX, &ageIn)};
    vkUpdateDescriptorSets(dev, (uint32_t)(sizeof(w) / sizeof(w[0])), w, 0, nullptr);
    cv2ScreenResetOnly = cv2ScreenResetOnly || cv2HistoryValid;   // the Sun shadow grids keep (recordCloudsV2 params)
    cv2HistoryValid = false;
}

void SatelliteSim::destroyCloudsV2Targets(VkDevice device)
{
    VkImageView *views[] = {&cv2NewView, &cv2NewDepthView, &cv2ResolvedView, &cv2ResolvedDepthView, &cv2HistoryView,
                            &cv2FullView, &cv2FullDepthView, &cv2FarView, &cv2AgeView, &cv2AgeHistView};
    VkImage *imgs[] = {&cv2NewImg, &cv2NewDepthImg, &cv2ResolvedImg, &cv2ResolvedDepthImg, &cv2HistoryImg,
                       &cv2FullImg, &cv2FullDepthImg, &cv2FarImg, &cv2AgeImg, &cv2AgeHistImg};
    VkDeviceMemory *mems[] = {&cv2NewMem, &cv2NewDepthMem, &cv2ResolvedMem, &cv2ResolvedDepthMem, &cv2HistoryMem,
                              &cv2FullMem, &cv2FullDepthMem, &cv2FarMem, &cv2AgeMem, &cv2AgeHistMem};
    for (int i = 0; i < 10; ++i)
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
                                                uint32_t wgX, uint32_t wgY, const std::string &resolveSpv,
                                                const std::string &lightningSpv, const std::string &compositeSpv)
{
    // Each override (harness `shaders reload march= resolve= lightning= composite=`) loads another SPV, so several
    // experiments can share one live app without overwriting the shipped SPVs; an empty one loads the shipped file.
    auto pick = [](const std::string &o, const char *def) { return o.empty() ? def : o.c_str(); };
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
    swap(cv2ResolvePipeline, pick(resolveSpv, "shaders/cloud_v2_resolve.comp.spv"), cv2ResolvePipeLayout, nullptr);
    swap(cv2TilesPipeline, "shaders/cloud_v2_tiles.comp.spv", cv2MarchPipeLayout, nullptr);
    swap(cv2LightningPipeline, pick(lightningSpv, "shaders/cloud_v2_lightning.comp.spv"), cv2MarchPipeLayout, nullptr);
    swap(cv2SunLutPipeline, "shaders/cloud_v2_sunlut.comp.spv", cv2MarchPipeLayout, nullptr);
    swap(cv2FroxPipeline, "shaders/cloud_v2_froxel.comp.spv", cv2MarchPipeLayout, nullptr);
    swap(cv2TshPipeline, "shaders/cloud_v2_terrsun.comp.spv", cv2MarchPipeLayout, nullptr);
    cv2SunLutKey[0] = -1.0f;   // bake it again with the new shader
    swap(cv2FarPipeline, "shaders/cloud_v2_far.comp.spv", cv2MarchPipeLayout, nullptr);
    if (cv2CombinePipeline)
        swap(cv2CombinePipeline, "shaders/cloud_v2_combine.comp.spv", cv2CombinePipeLayout, nullptr);
    for (int m = 0; m < 2; ++m)
    {
        const uint32_t spec[3] = {0u, 0u, (uint32_t)m};
        VkPipeline &cp = m == 0 ? cv2CdcPipeline : cv2CdcBakePipeline;
        try
        {
            VkPipeline np = makeComputePipeline(ctx, "shaders/cloud_v2_cdc.comp.spv", cv2MarchPipeLayout, spec, 3);
            if (cp) vkDestroyPipeline(ctx.device, cp, nullptr);
            cp = np;
        }
        catch (const std::exception &e)
        {
            err += std::string(e.what()) + "; ";
        }
    }
    ++cv2CdcEpoch;      // and the clipmap
    swap(cloudMarchPipeline, pick(compositeSpv, "shaders/cloud_march.comp.spv"), cloudMarchPipeLayout, nullptr);   // the composite
    cv2HistoryValid = false;
    stats = pipelineStatsLine(ctx, cv2MarchPipeline);
    // The composite's too (2026-10-21: its plume / dust pass is timed with composite= variants).
    if (const std::string cs = pipelineStatsLine(ctx, cloudMarchPipeline); !cs.empty())
        stats += "; composite: " + cs;
    return err;
}

// Harness `shaders reload sky=<spv> sky_taa=<spv>` (2026-10-24): the full sky pass from other fragment SPVs, so sky
// variants interleave in one live app like the cloud passes'. Same layouts and descriptor sets; the TAA images are
// recreated (history dropped). Not on a light tier (the full sky pipelines are deferred there).
std::string SatelliteSim::reloadSkyShaders(VulkanContext &ctx, const std::string &skySpv, const std::string &skyTaaSpv,
                                           std::string &stats)
{
    skyFragSpvOverride = skySpv;
    skyTaaFragSpvOverride = skyTaaSpv;
    if (fullSkyDeferred)
        return "the full sky pipelines are deferred (light tier)";
    vkDeviceWaitIdle(ctx.device);
    try
    {
        vkDestroyPipeline(ctx.device, skyBgPipeline, nullptr);
        skyBgPipeline = VK_NULL_HANDLE;
        vkDestroyPipeline(ctx.device, skyBgMinimalPipeline, nullptr);
        skyBgMinimalPipeline = VK_NULL_HANDLE;
        vkDestroyPipeline(ctx.device, skyBgLitePipeline, nullptr);
        skyBgLitePipeline = VK_NULL_HANDLE;
        createSkyBgPipeline(ctx);
        destroySkyLowResResources(ctx.device);
        createSkyLowResResources(ctx);
        destroySkyTaaResources(ctx.device);
        createSkyTaaResources(ctx);
    }
    catch (const std::exception &e)
    {
        return std::string(e.what());
    }
    stats = pipelineStatsLine(ctx, skyTaaPipeline ? skyTaaPipeline : skyBgPipeline);
    return "";
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
    if (cv2SunLutPipeline) vkDestroyPipeline(device, cv2SunLutPipeline, nullptr);
    cv2SunLutPipeline = VK_NULL_HANDLE;
    if (cv2FroxPipeline) vkDestroyPipeline(device, cv2FroxPipeline, nullptr);
    cv2FroxPipeline = VK_NULL_HANDLE;
    if (cv2TshPipeline) vkDestroyPipeline(device, cv2TshPipeline, nullptr);
    cv2TshPipeline = VK_NULL_HANDLE;
    if (cv2FarPipeline) vkDestroyPipeline(device, cv2FarPipeline, nullptr);
    cv2FarPipeline = VK_NULL_HANDLE;
    if (cv2CdcPipeline) vkDestroyPipeline(device, cv2CdcPipeline, nullptr);
    if (cv2CdcBakePipeline) vkDestroyPipeline(device, cv2CdcBakePipeline, nullptr);
    cv2CdcPipeline = cv2CdcBakePipeline = VK_NULL_HANDLE;
    if (cv2CdcBuf) vkDestroyBuffer(device, cv2CdcBuf, nullptr);
    if (cv2CdcMem) vkFreeMemory(device, cv2CdcMem, nullptr);
    cv2CdcBuf = VK_NULL_HANDLE; cv2CdcMem = VK_NULL_HANDLE;
    if (cv2SunLutView) vkDestroyImageView(device, cv2SunLutView, nullptr);
    if (cv2SunLutImg) vkDestroyImage(device, cv2SunLutImg, nullptr);
    if (cv2SunLutMem) vkFreeMemory(device, cv2SunLutMem, nullptr);
    cv2SunLutView = VK_NULL_HANDLE; cv2SunLutImg = VK_NULL_HANDLE; cv2SunLutMem = VK_NULL_HANDLE;
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
    if (cv2PassQuery) vkDestroyQueryPool(device, cv2PassQuery, nullptr);
    cv2PassQuery = VK_NULL_HANDLE;
    if (cv2FlashMem) { vkUnmapMemory(device, cv2FlashMem); cv2FlashMapped = nullptr; }
    if (cv2FlashBuf) vkDestroyBuffer(device, cv2FlashBuf, nullptr);
    if (cv2FlashMem) vkFreeMemory(device, cv2FlashMem, nullptr);
    cv2FlashBuf = VK_NULL_HANDLE; cv2FlashMem = VK_NULL_HANDLE;
    if (cv2BlueNoiseBuf) vkDestroyBuffer(device, cv2BlueNoiseBuf, nullptr);
    if (cv2BlueNoiseMem) vkFreeMemory(device, cv2BlueNoiseMem, nullptr);
    cv2BlueNoiseBuf = VK_NULL_HANDLE; cv2BlueNoiseMem = VK_NULL_HANDLE;
    destroyStormBuffer(device);
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
    VkImageView *views[] = {&cv2WeatherView, &cv2ShapeView, &cv2DetailView, &cv2MesoView, &cv2MorphView, &cv2MieView,
                            &cv2ShapeCapsView};
    VkImage *imgs[] = {&cv2WeatherImg, &cv2ShapeImg, &cv2DetailImg, &cv2MesoImg, &cv2MorphImg, &cv2MieImg, &cv2ShapeCapsImg};
    VkDeviceMemory *mems[] = {&cv2WeatherMem, &cv2ShapeMem, &cv2DetailMem, &cv2MesoMem, &cv2MorphMem, &cv2MieMem,
                              &cv2ShapeCapsMem};
    for (int i = 0; i < (int)(sizeof(views) / sizeof(views[0])); ++i)
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

// The clipmap's full buffer, on the first frame it is switched on (top of recordCompute: the last frame's fence has been
// waited on and nothing this frame has recorded the set yet). Stamp 0 everywhere: every cell reads unknown until baked.
void SatelliteSim::cv2EnsureCdcBuffer(VulkanContext &ctx)
{
    if (cv2CdcBufFull || cv2CdcSkip <= 0.5f || !cv2MarchDescSet)
        return;
    if (cv2CdcBuf) vkDestroyBuffer(ctx.device, cv2CdcBuf, nullptr);
    if (cv2CdcMem) vkFreeMemory(ctx.device, cv2CdcMem, nullptr);
    ctx.createBuffer(kCv2CdcBufBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                     VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, cv2CdcBuf, cv2CdcMem);
    VkCommandBuffer cmd = ctx.beginOneTimeCommands();
    vkCmdFillBuffer(cmd, cv2CdcBuf, 0, VK_WHOLE_SIZE, 0);
    ctx.endOneTimeCommands(cmd);
    VkDescriptorBufferInfo cdc{cv2CdcBuf, 0, VK_WHOLE_SIZE};
    VkWriteDescriptorSet w = bufferWrite(cv2MarchDescSet, 23, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &cdc);
    vkUpdateDescriptorSets(ctx.device, 1, &w, 0, nullptr);
    cv2CdcBufFull = true;
    ++cv2CdcEpoch;
    Log::line("clouds v2: cloud density clipmap allocated (" + std::to_string(kCv2CdcBufBytes >> 20) + " MB)");
}

// "shape_caps" (2026-10-22): when it changes, point every set that reads the shape volume at the other bake. Called at
// the top of recordCompute, before this frame records anything that binds these sets.
void SatelliteSim::cv2SyncShapeVolume(VulkanContext &ctx)
{
    if (cv2ShapeCaps == cv2ShapeCapsUsed || !cv2MarchDescSet)
        return;
    cv2ShapeCapsUsed = cv2ShapeCaps;
    VkDescriptorImageInfo shape{cv2RepeatSampler, cv2ShapeActiveView(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet w = imageWrite(cv2MarchDescSet, 3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &shape);
    vkUpdateDescriptorSets(ctx.device, 1, &w, 0, nullptr);
    writeCloudsV2ConsumerDescriptors(ctx);
    cv2HistoryValid = false;
}

// cloud_march.comp bindings 15-20 + 22 and beam_self_march.comp bindings 5-9. Called once both sets
// exist (init) and again after a resize (19/20 point at the recreated resolved images).
void SatelliteSim::writeCloudsV2ConsumerDescriptors(VulkanContext &ctx)
{
    const VkDescriptorType UBO = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, TEX = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    VkDescriptorBufferInfo v2Info{cv2ParamsBuf, 0, sizeof(GpuCloudV2Params)};
    VkDescriptorImageInfo weather{cv2RepeatSampler, cv2WeatherView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkDescriptorImageInfo shape{cv2RepeatSampler, cv2ShapeActiveView(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkDescriptorImageInfo meso{cv2RepeatSampler, cv2MesoView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkDescriptorImageInfo morph{cv2RepeatSampler, cv2MorphView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkDescriptorImageInfo res{cv2ClampSampler, cv2ResolvedView, VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorImageInfo resDepth{cv2ClampSampler, cv2ResolvedDepthView, VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorBufferInfo flashes{cv2FlashBuf, 0, VK_WHOLE_SIZE};
    VkDescriptorBufferInfo storms{stormBuf, 0, VK_WHOLE_SIZE};
    // The sky set's binding 7 (sat_sky.frag's flat layers, its SKY_ENV / SKY_LITE variants and Potato):
    // the weather cube, so every flat stand-in shows the same evolving map as the volumetric clouds.
    VkDescriptorImageInfo weatherCube{cv2RepeatSampler, cv2WeatherView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkDescriptorImageInfo farLayer{VK_NULL_HANDLE, cv2FarView, VK_IMAGE_LAYOUT_GENERAL};   // review 18
    VkDescriptorBufferInfo engineLights{kDustBuf, 0, VK_WHOLE_SIZE};   // the dust list's tail: the sea's light glints
    VkWriteDescriptorSet w[] = {
        imageWrite(skyDescSet, 7, TEX, &weatherCube),
        imageWrite(skyDescSet, 29, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &farLayer),
        bufferWrite(skyDescSet, 30, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &flashes),   // the Sun shadow cascade
        bufferWrite(skyDescSet, 31, UBO, &v2Info),
        bufferWrite(skyDescSet, 32, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &engineLights),
        bufferWrite(cloudMarchDescSet, 21, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &flashes),
        bufferWrite(cloudMarchDescSet, 15, UBO, &v2Info),
        imageWrite(cloudMarchDescSet, 16, TEX, &weather),
        imageWrite(cloudMarchDescSet, 17, TEX, &shape),
        imageWrite(cloudMarchDescSet, 18, TEX, &meso),
        imageWrite(cloudMarchDescSet, 19, TEX, &res),
        imageWrite(cloudMarchDescSet, 20, TEX, &resDepth),
        imageWrite(cloudMarchDescSet, 22, TEX, &morph),
        bufferWrite(beamSelfMarchDescSet, 5, UBO, &v2Info),
        imageWrite(beamSelfMarchDescSet, 6, TEX, &weather),
        imageWrite(beamSelfMarchDescSet, 7, TEX, &shape),
        imageWrite(beamSelfMarchDescSet, 8, TEX, &meso),
        imageWrite(beamSelfMarchDescSet, 9, TEX, &morph),
        bufferWrite(beamSelfMarchDescSet, 10, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &storms)};
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
    double groundGpu = terrainFrameMapped ? (double)terrainFrameMapped[0] : 0.0;
    // 2026-10-22 (r1022 orbit, "eye_height_now"): terrainFrame.x is max(ground, height offset) of LAST frame, while every
    // v2 pass builds its rays from THIS frame's (scene_depth.comp writes it before them). Climbing with Q from orbit
    // (4-15 km a frame) the CPU's eye, and everything integrated from it (the resolve's obsDelta, the Sun volume's lattice
    // and window centre), lagged the GPU's by one frame's climb. Last frame's GROUND (.y, no offset: it moves only with
    // the ground under the observer) with this frame's offset is the GPU's value up to the ground's own change.
    if (cv2EyeHeightNow > 0.5f && terrainFrameMapped)   // (over the sea the ground reads 0: the offset alone)
        groundGpu = std::max((double)terrainFrameMapped[1], std::max(0.0, (double)obsHeightOffset));
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
                    cv2WeatherWarpKm, cv2MidAmount, cv2StormScale, cv2StormDetail,
                    cv2Anvil, cv2BaseRoughness, cv2HighAmount, cv2HighDensity, cv2CirrusStretch,
                    cv2RainAmount, cv2OpticsGain, cv2CirrusPeriodM, cv2MidPeriodM, cv2MidDensity,
                    cv2CloudSunRayleigh, cv2TwilightSky, cv2FlowWarp, cv2FlowPeriodKm, cv2LayerSpread,
                    cv2TopHeavy, cv2TowerTop, cv2CirrusFieldKm, cv2CirrusFlow, cv2BaseFlatness, cv2CuLobesBottom, cv2CuLobesTop,
                    cv2CbColumns, cv2CbSpacingKm, cv2CbRadiusKm, cv2CbCumulusTopKm, cv2CbWaist, cv2CbFlare,
                    cv2CbHeadDriftKm, cv2CbLobes, cv2CbHeadLobes, cv2AnvilPuff, cv2AnvilLiftM, cv2CbSparsity, cv2CbOvershootKm, cv2AnvilThickKm, cv2AnvilHangKm, cv2CbCumulusReachKm, cv2LightLodFootprintM, cv2LightningRate, cv2CbFill, cv2CbCumulusVar, cv2FogAmount, cv2FogDepthM, cv2FogDensity, cv2DustAmount,
                    cv2DustHeightM, cv2DustDensity, (float)(debugDisableMask & 2048u),
                    cv2LightFootprint, cv2SmallCumulus, cv2ErosionVar, cv2LobeVar, cv2CirrusDirWarp, cv2AsDensity, cv2MidThick, cv2MidShadow,
                    cv2MidFieldKm, cv2MidCover, cv2MsOctaves, cv2LobeOcclusion,
                    cv2CuHeightVar, cv2CuBreakup, cv2CuHeadroom, cv2CuFineLobes, cv2CbFineLobes, cv2CuWisps})
        mix(v);
    for (const GpuCloudV2Type &t : cv2Types)
        for (int k = 0; k < 4; ++k)
        {
            mix(t.alt[k]);
            mix(t.shape[k]);
            mix(t.look[k]);
        }

    const uint64_t hField = h;      // what the clouds ARE; the rate below only changes how the screen is marched
    mix(cv2FullRateAboveKm);
    mix(cv2CdcSkip);

    const bool nearPrev = glm::length(eye - cv2PrevEye) < 20000.0 && std::abs(simT - cv2PrevSimT) < 600.0;
    bool valid = cv2HistoryValid && h == cv2SettingsHash && nearPrev;
    // The Sun shadow grids (below) survive what only drops the SCREEN history: the HQ photo recreates the screen
    // images and switches the march to full rate, and with the grids reset by that the photo's first ~40 frames
    // had no far cloud shadows and no god rays (user report, 2026-10-10).
    const bool lightValid = (cv2HistoryValid || cv2ScreenResetOnly) && hField == cv2FieldHash && nearPrev;
    cv2ScreenResetOnly = false;

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
                         std::clamp(cv2CbHeadLobes, 0.0f, 1.5f),
                         // w: the noise LOD's pixel = the 100%-scale one (computeHalfExtent follows the render
                         // scale): at 50% the coarser pixel filtered the cumulus texture away.
                         // S9: full-res clouds keep the HALF-res pixel's LOD (w = 2): at the full-res pixel the sub-pixel
                         // noise flickered in motion from orbit (edge-flicker share 0.23% vs 0.065%) for +2 ms at 58 km;
                         // the cloud edges stay full-res sharp either way (harness_runs/s0/lod_crops.png).
                         (float)cv2HalfH / (float)std::max(1u, (ctx.swapExtent.height + 1) / 2));

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
    p.anchorMorph = anchor((double)std::clamp(cv2MorphPeriodKm, 40.0f, 2000.0f) * 1000.0, 0.6);   // review 17
    p.morph = glm::vec4(std::clamp(cv2MorphOrbit, 0.0f, 1.0f), std::clamp(cv2ImageryShare, 0.0f, 1.0f),
                        std::clamp(cv2FoveaRadius, 0.0f, 2.0f),   // review 22 (was "Morphology from (m/px)")
                        std::exp2(-std::clamp(cv2MorphBreakup, 0.0f, 3.0f)));
    p.farLight = glm::vec4(cv2FarKeyGain, cv2FarSkyGain, std::clamp(cv2CuLobesBottom, 0.0f, 1.0f),
                           std::clamp(cv2CuLobesTop, 0.0f, 1.0f));   // zw: review 22's cumulus lobes
    p.farTune = glm::vec4(std::clamp(cv2FarSlant, 0.0f, 4.0f), std::clamp(cv2FarCoverBias, -0.5f, 0.5f),
                          std::clamp(cv2FarDensity, 0.0f, 8.0f), std::clamp(cv2FarSoftness, 0.1f, 8.0f));
    {
        // The mid layer reads the morphology at 1.7x its frequency (clouds_v2.glsl). Scaling the shader's
        // frac(anchor) by 1.7 jumped the pattern by 0.7 of a period whenever the observer crossed a period
        // plane (an instant change of the Ac/As texture on moving ~1 km): reduce the scaled anchor here.
        const double mp = (double)std::clamp(cv2MorphPeriodKm, 40.0f, 2000.0f) * 1000.0 / 1.7;
        const glm::dvec3 a = (sea + windDir * ((double)cv2WindMps * 0.6 * simT)) / mp;
        p.farTune2 = glm::vec4(std::clamp(cv2FarLowSun, 0.0f, 4.0f),
                               (float)fracPos(a.x), (float)fracPos(a.y), (float)fracPos(a.z));
    }
    p.anchorCell = anchor(cv2CellPeriodM, 0.85);
    const double stormScale = std::clamp((double)cv2StormScale, 0.25, 16.0);
    p.anchorStorm = anchor(cv2ShapePeriodM * stormScale, 1.0);
    p.anchorStormDetail = anchor(cv2DetailPeriodM * stormScale, 1.6);
    // The low shape volume's frame (2026-10-04): the volume tiles, and where the local horizontal plane holds a short
    // lattice direction of the tile the clouds repeat in rows (clouds_v2.glsl). Four fixed rotations (the identity and
    // three chosen offline so that one of them is always clear): the identity where it is clear enough at the
    // observer's (drifted) up (so most places look as before), else the clearest. A pure function of position and
    // time; a switch reshapes the lobes (the history eases it), never where the clouds are.
    {
        static const double kFrames[4][3][3] = {
            {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}},
            {{0.45358556, -0.17302729, 0.87425494}, {0.71791067, 0.65220034, -0.24339062}, {-0.52807615, 0.73803541, 0.42004679}},
            {{0.24436528, -0.52796272, 0.81335169}, {-0.93912632, -0.33776453, 0.06290365}, {0.24151057, -0.77921145, -0.57836161}},
            {{-0.68377617, -0.54298685, -0.48745814}, {0.66749276, -0.73533171, -0.11722071}, {-0.29479412, -0.40552751, 0.86524209}}};
        auto mulF = [&](int f, const glm::dvec3 &v)
        {
            const auto &R = kFrames[f];
            return glm::dvec3(R[0][0] * v.x + R[0][1] * v.y + R[0][2] * v.z, R[1][0] * v.x + R[1][1] * v.y + R[1][2] * v.z,
                              R[2][0] * v.x + R[2][1] * v.y + R[2][2] * v.z);
        };
        // How far the tile's short lattice directions are from the horizontal plane: min |u . v| over the axes, face
        // and body diagonals, the longer (longer-period, less visible) ones counted a little less.
        auto clearance = [](const glm::dvec3 &u)
        {
            double m = 1.0;
            for (int x = -1; x <= 1; ++x)
                for (int y = -1; y <= 1; ++y)
                    for (int z = 0; z <= 1; ++z)
                    {
                        if (z == 0 && (y < 0 || (y == 0 && x <= 0))) continue;   // one of each +-v pair
                        const glm::dvec3 v(x, y, z);
                        const double n = glm::length(v);
                        m = std::min(m, std::abs(glm::dot(u, v)) / n * (n > 1.5 ? 1.6 : n > 1.2 ? 1.3 : 1.0));
                    }
            return m;
        };
        const glm::dvec3 u = glm::normalize(sea);
        double m[4];
        for (int f = 0; f < 4; ++f) m[f] = clearance(mulF(f, u));
        int f1 = 0;
        if (m[0] < 0.05)
            for (int f = 1; f < 4; ++f) if (m[f] > m[f1]) f1 = f;
        for (int c = 0; c < 3; ++c)
            p.shapeRotA[c] = glm::vec4((float)kFrames[f1][0][c], (float)kFrames[f1][1][c], (float)kFrames[f1][2][c], 0.0f);
        auto anchorF = [&](int f, double periodM, double windMul)
        {
            const glm::dvec3 a = mulF(f, sea + windDir * ((double)cv2WindMps * windMul * simT)) / periodM;
            return glm::vec4((float)fracPos(a.x), (float)fracPos(a.y), (float)fracPos(a.z), (float)(1.0 / periodM));
        };
        p.anchorShapeA = anchorF(f1, cv2ShapePeriodM, 1.0);
        p.anchorStormA = anchorF(f1, cv2ShapePeriodM * stormScale, 1.0);
        cv2ShapeFrame = f1;
    }
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
                           std::clamp(cv2TopHeavy, 0.0f, 2.0f), std::clamp(cv2TowerTop, 0.3f, 0.95f));
    }

    {
        // The Cb column lattice: 64 cells per period, so a cell's id mod 64 (its hash) is stable as the
        // observer moves; carried with the cells (0.85 of the wind), like the field they rise from.
        const double cellM = std::clamp((double)cv2CbSpacingKm, 8.0, 120.0) * 1000.0;
        // The towers' own lobe noise (2026-10-10; the 3D lattice anchorCol held is gone since pass 16): the storm
        // shape volume WITHOUT the wind's slide, and in w the phase of the towers' flow (clouds_v2.glsl
        // cv2ColumnSigma: the lobes rise up a tower and spread out in its head, over kColFlowSpan of a period per
        // cycle, at 1.5 x "Wind"). The wind is one fixed direction in the drifted ECEF frame, so at a given place
        // the noise slid through the (stationary) towers at whatever angle that makes with the local vertical: at
        // 25 N 134 W, 56% of it straight DOWN -- the user's "clouds falling down their columns".
        const double kColFlowSpan = 0.35;   // = kCv2ColFlowSpan
        const double stormP = cv2ShapePeriodM * stormScale;
        p.anchorCol = anchor(stormP, 0.0);
        p.anchorCol.w = (float)fracPos(1.5 * (double)cv2WindMps * simT / (kColFlowSpan * stormP));
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
    // Light LOD (cloud_v2_march.comp), only for a light march up to 3 km: its second step is one sample over the
    // rest of the march, and at 8 km that is a 7.5-km all-or-nothing sample (the grain of 2026-10-11; a saved
    // settings.json still carries the old 16 m).
    p.column3.y = cv2LightLenM <= 3000.0f ? std::max(cv2LightLodFootprintM, 0.0f) : 0.0f;
    // Lightning: its schedule runs on SIM time (deterministic, reversible), wrapped where a float still
    // resolves ~10 ms (a flash in progress at the wrap is cut; once per ~28 h of sim time).
    // The light volume: +-"God ray range" about the eye (a low Sun's shafts come from storms hundreds of
    // km away), sea level to 16 km, four of its 32 levels per frame.
    // (xyz were the light volume's, deleted 2026-10-13; w is read: "God rays".)
    p.lightVol = glm::vec4(0.0f, std::clamp(cv2GodrayRangeKm, 50.0f, 1500.0f) * 1000.0f, 16000.0f, std::max(cv2Godrays, 0.0f));
    // z: the eye moved (over 1 m since the last frame): the march switches its ray jitter from IGN to
    // blue noise (cloud_v2_march.comp). It was over 150 m (fast flight) until review 7: at a walking or
    // hover speed the moving history weight (0.7, the user's 1.0) let IGN's regular structure through as
    // a dotted honeycomb over lit cloud tops (user snapshot 4).
    p.misc2 = glm::vec4(std::clamp(cv2LightningSprites, 0.0f, 1.0f), 1.0f,
                        glm::length(eye - cv2PrevEye) > 1.0 ? 1.0f : 0.0f, 0.0f);
    p.lightning = glm::vec4(std::max(cv2LightningRate, 0.0f), std::max(cv2LightningGlow, 0.0f),
                            std::max(cv2LightningBolt, 0.0f), (float)std::fmod((double)simDayJ2000 * 86400.0 + simSecInDay, 100000.0));
    {
        // yzw: the storm detail volume without the wind's slide (the towers' erosion; see anchorCol).
        const glm::vec4 ad = anchor(cv2DetailPeriodM * stormScale, 0.0);
        p.lightning2 = glm::vec4(std::max(cv2LightningStormRate, 0.0f), ad.x, ad.y, ad.z);
    }

    static const int kOffsets[4][2] = {{0, 0}, {1, 1}, {1, 0}, {0, 1}};
    const int *o = kOffsets[cv2Frame & 3u];
    p.frame = glm::vec4((float)(cv2Frame & 0xFFFFu), valid ? 1.0f : 0.0f, (float)o[0], (float)o[1]);
    p.march = glm::vec4(cv2StepBaseM, cv2StepGrowth, cv2StepMaxM, cv2MaxDistKm * 1000.0f);
    p.light = glm::vec4(cv2LightLenM, std::round(std::clamp(cv2LightSteps, 1.0f, 12.0f)), cv2MsExtinction,
                        cv2MsStrength);
    p.look = glm::vec4(cv2Coverage, cv2Density, cv2Detail, cv2AmbientGain);
    // Review 18: from high orbit the still weight falls to 40% (from 300 km to 3000 km of altitude). A
    // still orbital view changes slowly, and each visit's ray lands at another point of a 5-20 km texel
    // whose small clouds it hits or misses: at 0.05 the history kept that as flicker on the scattered
    // puffs (MEO, the user's report).
    const float hwAlt = 1.0f - 0.6f * (float)std::clamp((std::log10(std::max(eyeH, 1.0)) - 5.477) / 1.0, 0.0, 1.0);
    p.look2 = glm::vec4(cv2BounceGain, cv2SunGain, cv2Powder,
                        cv2HqHistWeight > 0.0f ? cv2HqHistWeight : cv2HistoryWeight * hwAlt);
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
    {   // The Sun, Earth-fixed (the Earth rotation angle, as the weather bake uses).
        const double g = earthRotationAngle(simT);
        const glm::dvec3 si = glm::dvec3(sunDirECI);
        p.sunE = glm::vec4(glm::vec3(glm::normalize(glm::dvec3(si.x * std::cos(g) + si.y * std::sin(g),
                                                               -si.x * std::sin(g) + si.y * std::cos(g), si.z))),
                           std::max(cv2IceFogDensity, 0.0f));   // w: the ice fog's extinction
    }
    {
        // The Sun's axes across its direction: a (horizontal) and b (cloud_lightning.glsl cv2SunGridAxes).
        const glm::dvec3 s = glm::normalize(glm::dvec3(p.sunE));
        const double sinE = glm::dot(s, up);
        const glm::dvec3 east = glm::normalize(glm::cross(glm::dvec3(0.0, 0.0, 1.0), up));
        const glm::dvec3 ga = glm::normalize(glm::cross(s, std::abs(sinE) > 0.99 ? east : up));   // = cv2SunGridAxes
        const glm::dvec3 gb = glm::cross(s, ga);
        auto modP = [](double v, double m) { v = std::fmod(v, m); return (float)(v < 0.0 ? v + m : v); };
        // From orbit the sky's air keeps no cloud shadow (the god rays: not measured above 60 km).
        if (eyeH >= 60000.0) p.lightVol.w = 0.0f;
        const double cell1 = std::clamp((double)cv2ShadowRing1CellM, 50.0, 2000.0);   // "Sun shadow cell (m)"
        p.tune = glm::vec4(std::clamp(cv2SmallCumulus, 0.0f, 0.5f), std::max(cv2ErosionVar, 0.0f), std::max(cv2LobeVar, 0.0f),
                           std::clamp(cv2CirrusDirWarp, 0.0f, 1.3f));
        p.mid2 = glm::vec4(std::max(cv2AsDensity, 0.0f), std::clamp(cv2MidThick, 0.1f, 4.0f),
                           1.0f - std::clamp(cv2MidShadow, 0.0f, 1.0f), 0.057f * cv2MidCover);
        p.cu  = glm::vec4(std::clamp(cv2CuHeightVar, 0.0f, 1.0f), std::clamp(cv2CuBreakup, 0.0f, 1.0f),
                          std::clamp(cv2CuHeadroom, 0.0f, 2.0f), std::clamp(cv2CuFineLobes, 0.0f, 2.0f));
        // An HQ export's running mean must not be floored at 1 / (the accumulation cap): the cap goes to 256 there.
        p.samp = glm::vec4(cv2HqHistWeight > 0.0f && cv2SampAccum > 0.0f ? 256.0f : std::clamp(cv2SampAccum, 0.0f, 256.0f),
                           std::clamp(cv2SampNewSmooth, 0.0f, 1.0f),
                           std::clamp(cv2SampClampSd, 0.25f, 8.0f), std::clamp(cv2LightFootprint, 0.0f, 0.5f));

        // The Sun transmittance VOLUME (S3, 2026-10-22; cloud_lightning.glsl cv2Stv): the clouds' one Sun shadow. 4 levels
        // of 128 x 128 rays across the Sun (cells "Sun shadow cell" x 1, 4, 16, 64), knots by HEIGHT (0 .. the shell top +
        // 1 km). Each level's lattice coordinate is INTEGRATED from the eye's motion relative to the clouds (the drift turned
        // out, as the resolve's obsDelta), so a ray baked rounds ago still stands where its clouds are while the eye flies
        // and under time warp. A level starting to fill (or a new cell / knot range) is cleared first, so its slots read
        // invalid until baked and the next level out stands in. On while the march draws clouds.
        {
            const bool stvOn = sinE > -0.1 && cloudFarBlend() < 0.999f && cv2SunVolumeOn > 0.5f;   // (sun_volume 0: a diagnostic)
            const double hHi = (double)hi + 1000.0;
            const glm::dvec3 dEye = rotD(eye, cDD, sDD) - cv2PrevEye;
            glm::vec4 lat[4];
            for (int lv = 0; lv < 4; ++lv)
            {
                const double cellL = cell1 * std::pow(4.0, lv);
                if (cv2StvCellUsed[lv] != (float)cellL || std::abs(hHi - cv2StvHUsed) > 0.01 * hHi)
                    cv2StvFrames[lv] = 0;
                if (!stvOn || !lightValid)
                    cv2StvFrames[lv] = 0;
                else
                {
                    if (cv2StvFrames[lv] == 0)
                    {
                        std::memset(static_cast<char *>(cv2FlashMapped) + kCv2StvOffset + (VkDeviceSize)lv * kCv2StvLevelBytes, 0,
                                    kCv2StvLevelBytes);
                        cv2StvCellUsed[lv] = (float)cellL;
                    }
                    cv2StvLatA[lv] = std::fmod(cv2StvLatA[lv] + glm::dot(dEye, ga) / cellL, 128.0 * 1024.0);
                    cv2StvLatB[lv] = std::fmod(cv2StvLatB[lv] + glm::dot(dEye, gb) / cellL, 128.0 * 1024.0);
                    ++cv2StvFrames[lv];
                }
                // In over its first round (level 0: 32 frames, the others 64).
                const float fill = stvOn ? std::clamp((float)cv2StvFrames[lv] / (lv == 0 ? 16.0f : 32.0f), 0.0f, 1.0f) : 0.0f;
                lat[lv] = glm::vec4(modP(cv2StvLatA[lv], 1024.0), modP(cv2StvLatB[lv], 1024.0), (float)cellL, fill);
            }
            if (stvOn) cv2StvHUsed = hHi;
            p.stvL0 = lat[0]; p.stvL1 = lat[1]; p.stvL2 = lat[2]; p.stvL3 = lat[3];
            // The windows' centre (m across the Sun from the eye): the eye, or from above mid-shell the point under it there.
            const double hC = std::min(eyeH, 0.5 * hHi);
            const float wcA = (float)((hC - eyeH) * glm::dot(up, ga)), wcB = (float)((hC - eyeH) * glm::dot(up, gb));
            p.stvH = glm::vec4(0.0f, (float)hHi, stvOn ? 1.0f : 0.0f, wcA);
            // z: levels 0-1 baked twice (lightning workgroups 10-17) while they catch up: moving over half a level-0 cell a
            // frame, the 128 frames after, and the first 64 frames of a fill.
            const glm::vec2 v0((float)(glm::dot(dEye, ga) / cell1), (float)(glm::dot(dEye, gb) / cell1));
            cv2StvBoost = (stvOn && (glm::length(v0) > 0.5f || cv2StvFrames[0] < 64 || cv2StvFrames[1] < 64))
                              ? 128 : std::max(cv2StvBoost - 1, 0);
            // z's fraction: the smallest weight a ray's running mean gives a new bake once it has three. The bakes are
            // jittered, and at a fixed 1/4 the stored shadow kept swinging by a bake's noise forever: with time PAUSED the
            // ground under an overcast wandered +-2-3 levels over tens of frames, cumulus shadows' edges shimmered
            // (harness_runs/suite/ovc_trace). It falls to 1/32 as far as the clouds stand still: their change over one
            // level-0 round (32 frames) taken as ~20 m/s of wind and evolution x the sim time, against the cell.
            const double roundShift = 20.0 * std::abs(dSimT) * 32.0;
            const float wMin = (float)std::clamp(1.0 / 32.0 + 2.0 * roundShift / cell1, 1.0 / 32.0, 0.25);
            p.stvV = glm::vec4(v0.x, v0.y, (cv2StvBoost > 0 ? 1.0f : 0.0f) + std::min(wMin, 0.25f), wcB);
            cv2StvOnNow = stvOn;
        }
        {
            // The terrain Sun-horizon map (cloud_v2_terrsun.comp, cloud_lightning.glsl cv2TerrSunT): 4 levels of 128 x 128
            // lines toward the Sun at the volume's cells, snapped to whole cells of the eye's ECEF position across the Sun
            // (so a line stays on the same ground while the eye moves; rebaked whole every frame). Only while the bake is
            // dispatched (recordCloudsV2: not with the clouds off or at full far-layer blend), the Sun is up at the eye,
            // below 60 km, and the terrain is drawn.
            // Nor over open ocean (2026-10-23, perf): with no terrain in the lines' reach (+-1024 km across the Sun, from
            // 450 km past the eye to where a line toward the Sun rises above the terrain's ceiling) every line meets only
            // the sea, i.e. the Earth's own shadow, which the sky and the clouds already take: the bake (~0.3 ms) changed
            // nothing there. "Terrain" = 100 m or more in the CPU DEM in that box (+ 60 km), or any land within 60 km (the
            // user's tropical storm views had atolls of a few metres in reach). Re-checked every 25 km or 1 deg of Sun.
            // A jump or fast flight (> 200 km since last frame): baked, and searched again on the first slow frame after it.
            const double tshStep = glm::length(up - tshPrevUp_) * 6371000.0;
            tshPrevUp_ = up;
            if (tshStep > 200000.0)
            {
                tshLandNear_ = true;
                tshLandCheckUp_ = glm::dvec3(0.0);
            }
            else if (glm::length(up - tshLandCheckUp_) * 6371000.0 > 25000.0 || glm::dot(s, tshLandCheckSun_) < 0.99985)
            {
                tshLandCheckUp_ = up;
                tshLandCheckSun_ = s;
                const double ceilM = 9400.0, R = 6371000.0, se = std::max(sinE, 0.0);
                const double ahead = std::min(R * (-se + std::sqrt(se * se + 2.0 * ceilM / R)), 2500000.0);
                tshLandNear_ = terrainInSunBox(up, s, ga, 1024000.0 + 60000.0, 450000.0 + 60000.0, ahead + 60000.0, 100.0f) ||
                               landWithin(std::asin(std::clamp(up.z, -1.0, 1.0)), std::atan2(up.y, up.x), 60000.0);
            }
            const bool tshOn = cv2TerrainGodrays > 0.5f && cv2TshPipeline && !cloudsV2Off() && cloudFarBlend() < 0.999f
                               && sinE > -0.05 && eyeH < 60000.0 && (debugDisableMask & 1u) == 0u && tshLandNear_;
            const double eA = glm::dot(eye, ga), eB = glm::dot(eye, gb);
            glm::vec4 t[4];
            for (int lv = 0; lv < 4; ++lv)
            {
                const double cellL = cell1 * std::pow(4.0, lv);
                const double fa = eA / cellL - std::floor(eA / cellL), fb = eB / cellL - std::floor(eB / cellL);
                t[lv] = glm::vec4((float)fa, (float)fb, (float)cellL, tshOn ? 1.0f : 0.0f);
            }
            p.tsh0 = t[0]; p.tsh1 = t[1]; p.tsh2 = t[2]; p.tsh3 = t[3];
            cv2TshOnNow = tshOn;
        }
        // mid3.w: "Haze near clouds" (the sky pass). cu2.w: how far the resolve may take a still view's history unclamped
        // (cloud_v2_resolve.comp `uK`): not while the Sun shadow fills after a reset, its shadows arriving over ~40 frames
        // darkened the clouds over seconds at the still weight (load: 18% of pixels past 16 levels at frame 20, was 0.7%).
        // (S3: level 1's round is 64 frames.)
        const int lightFill = cv2StvOnNow ? cv2StvFrames[1] * 2 : 1000;
        p.mid3 = glm::vec4(cv2MidFieldKm > 1.0f ? 1.0f / (1000.0f * cv2MidFieldKm) : 0.0f,
                           std::round(std::clamp(cv2MsOctaves, 1.0f, 8.0f)), std::clamp(cv2LobeOcclusion, 0.0f, 1.0f),
                           std::max(cv2HazeAloft, 0.0f));
        p.cu2 = glm::vec4(std::clamp(cv2CbFineLobes, 0.0f, 2.0f), std::clamp(cv2CuWisps, 0.0f, 1.0f),
                          std::clamp(cv2SilverLining, 0.0f, 4.0f),
                          std::clamp(((float)lightFill - 64.0f) / 32.0f, 0.0f, 1.0f));

        // The cloud density clipmap's plane (cv2_cdc.glsl): a gnomonic plane tangent at an anchor fixed in the DRIFTED
        // frame (the clouds stand still in it), so nothing is integrated: the eye's coordinate is a projection. A new
        // anchor (> 1500 km away) starts a new epoch. (It was the layer shadow map's plane, deleted 2026-10-22.)
        const glm::dvec3 eyeD = rotD(up, cD, sD);
        bool cdcNewAnchor = false;
        if (!cv2CdcAnchorSet || glm::dot(eyeD, cv2CdcAnchor) < std::cos(1500000.0 / R))
        {
            cv2CdcAnchor = eyeD;
            cv2CdcAnchorSet = true;
            cdcNewAnchor = true;
        }
        {
            const glm::dvec3 A = cv2CdcAnchor;
            const glm::dvec3 Ed = glm::normalize(glm::cross(glm::dvec3(0.0, 0.0, 1.0), std::abs(A.z) > 0.999 ? glm::dvec3(1.0, 0.0, 0.0) : A));
            const glm::dvec3 Nd = glm::cross(A, Ed);
            // ... into the observer's ENU frame: undrift (Earth-fixed ECEF), then onto east / north / up.
            const glm::dvec3 ex = glm::normalize(glm::cross(glm::dvec3(0.0, 0.0, 1.0), up)), ey = glm::cross(up, ex);
            auto toEnu = [&](const glm::dvec3 &vd)
            {
                const glm::dvec3 v = rotD(vd, cD, -sD);
                return glm::vec3((float)glm::dot(v, ex), (float)glm::dot(v, ey), (float)glm::dot(v, up));
            };
            const double ue = R * glm::dot(eyeD, Ed) / glm::dot(eyeD, A), ve = R * glm::dot(eyeD, Nd) / glm::dot(eyeD, A);
            p.layA = glm::vec4(toEnu(A), 0.0f);
            p.layE = glm::vec4(toEnu(Ed), 0.0f);
            p.layN = glm::vec4(toEnu(Nd), 0.0f);
            // The cloud density clipmap (EXPERIMENT, cv2_cdc.glsl), on the same plane. A new epoch (the anchor, the field,
            // a time jump or a reset) invalidates every cell. Each level's cells expire after half a cell of wind slide at
            // 40 m/s of sim time (staggered per cell); paused, nothing expires.
            const uint32_t cdcEpoch0 = cv2CdcEpoch;
            if (!lightValid || cdcNewAnchor || cv2CdcFpUsed != cv2CdcFp || cv2CdcSkipUsed != cv2CdcSkip
                || cv2CdcLightUsed != cv2CdcLight || cv2CdcLightLevelsUsed != cv2CdcLightLevels || cv2CdcMaxLevelUsed != cv2CdcMaxLevel)
                ++cv2CdcEpoch;
            cv2CdcFpUsed = cv2CdcFp;
            cv2CdcLightUsed = cv2CdcLight;
            cv2CdcLightLevelsUsed = cv2CdcLightLevels;
            cv2CdcSkipUsed = cv2CdcSkip;
            cv2CdcMaxLevelUsed = cv2CdcMaxLevel;
            const double cdcCell0 = 100.0, cdcWind = 40.0;
            // Only up to cdc_max_rate x real time: at 10x the cells expire ~10x as fast as a frame's budget bakes them, most
            // read unknown, and the bake + lookups cost more than they save (+2 ms at 58 km, harness_runs/s0/run13).
            const float rate = timePaused ? 0.0f : kTimeScales[std::clamp(timeScaleIdx, 0, (int)(sizeof(kTimeScales) / sizeof(kTimeScales[0])) - 1)];
            cv2CdcActive = cv2CdcSkip > 0.5f && rate <= cv2CdcMaxRate && cv2CdcBufFull;
            // The motion guard (SatelliteSim.h cv2CdcMotionGuard): a cell's bake read the field at the footprint seen from
            // where the eye WAS, and paused it never expires. Its distance scale: the nearest cloud is at least this far
            // (from orbit the height above the shell; near the ground 20 km, so normal and boosted low flight keep it).
            // Off while the eye moves > 0.4% of it a frame (back after 60 slow frames, in a new epoch), and a new epoch
            // once it has moved a quarter of it (>= ~60 frames apart, so the 3-bit epoch never wraps onto unbaked cells).
            // Matrix run 1 (r1022): climbing 500 -> 1745 km the march skipped every cloud mass the 500-km bake had seen as
            // gaps between puffs (mx_alt500km-rise, -1.0 signed, 2.5% px past 16 levels).
            if (cv2CdcMotionGuard > 0.5f)
            {
                const double cdcS = std::max(eyeH - (double)hi, 0.0) + 20000.0;
                const double cdcStep = glm::length(eye - cv2PrevEye);
                // Any step counts, history or not (2026-10-22, issue 20261022_210358): past 20 km a frame the cloud history
                // is invalid (nearPrev), and gated on it the fastest flight counted as SLOW: after 60 such frames the
                // clipmap came back on mid-flight with cells baked from far behind, and skipped the distant cloud masses
                // (far band -30% of cloud while moving at ~700 km/s from 1278 km, harness_runs/fl1022c).
                cv2CdcSlowFrames = (cdcStep > 0.004 * cdcS) ? 0 : std::min(cv2CdcSlowFrames + 1, 1000);
                if (cv2CdcSlowFrames < 60)
                    cv2CdcActive = false;
                if (cv2CdcActive && (!cv2CdcWasActive || glm::length(eye - cv2CdcRefEye) > 0.25 * cv2CdcRefS))
                    ++cv2CdcEpoch;
                if (cv2CdcEpoch != cdcEpoch0 || !cv2CdcActive)
                {
                    cv2CdcRefEye = eye;
                    cv2CdcRefS = cdcS;
                }
            }
            cv2CdcWasActive = cv2CdcActive;
            p.cdcA = glm::vec4((float)ue, (float)ve, (float)cdcCell0, cv2CdcActive ? std::round(cv2CdcSkip) : 0.0f);
            p.cdcB = glm::vec4((float)(cv2CdcEpoch & 7u), (cv2CdcFp < 0.0f ? std::clamp(cv2CdcFp, -8.0f, -0.05f) : std::clamp(cv2CdcFp, 0.05f, 8.0f)), std::max(cv2CdcBudget, 0.0f),
                               std::clamp(cv2CdcMaxLevel, 0.0f, 8.0f));
            float ph[8];
            for (int k = 0; k < 8; ++k)
                ph[k] = (float)std::fmod(simT / (0.5 * cdcCell0 * std::ldexp(1.0, k) / cdcWind), 4096.0);
            p.cdcP0 = glm::vec4(ph[0], ph[1], ph[2], ph[3]);
            p.cdcC = glm::vec4(cv2CdcActive && cv2CdcLight > 0.5f ? 1.0f : 0.0f, std::clamp(cv2CdcLightLevels, 0.0f, 8.0f),
                               std::max(cv2CdcNearM, 0.0f), 0.0f);
            // The god rays' froxel volume: 64 slices from 50 m to 400 km.
            // w: the frame's index in the froxels' sample sequence (cv2FroxJitter), -1 = fixed at the cells' centres when nothing
            // averages frames (no sky TAA: it would flicker).
            p.frox = glm::vec4(cv2GodrayFroxels > 0.5f ? 1.0f : 0.0f, 50.0f, (float)(std::log(400000.0 / 50.0) / kCv2FroxZ),
                               skyTaaWanted() ? (float)(cv2FroxFrame++ % 4096u) : -1.0f);
            {   // The Mie table's row for the droplets' effective radius, between the tabulated radii.
                float row = 0.0f;
                const int nr = (int)cv2MieReff.size();
                if (nr > 1)
                {
                    const float r = std::clamp(cv2DropletReffUm, cv2MieReff.front(), cv2MieReff.back());
                    int i = 0;
                    while (i + 2 < nr && cv2MieReff[i + 1] < r) ++i;
                    row = (float)i + (r - cv2MieReff[i]) / std::max(cv2MieReff[i + 1] - cv2MieReff[i], 1e-3f);
                }
                // y: the phase row, w: the same radius's structure row (the second half of the texture), z "Mie optics".
                const float nrows = 2.0f * (float)std::max(nr, 1);
                p.mie = glm::vec4(nr > 0 ? std::clamp(cv2MiePhase, 0.0f, 1.0f) : 0.0f, (row + 0.5f) / nrows,
                                  nr > 0 ? std::clamp(cv2MieOptics, 0.0f, 2.0f) : 0.0f, (row + 0.5f + (float)nr) / nrows);
            }
            p.cdcP1 = glm::vec4(ph[4], ph[5], ph[6], ph[7]);
        }
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
        // w (2026-10-06; was the old drop lattice's distance, unread): the precipitation SHAFTS' liquid share, from the
        // air 500 m over the ground under the eye (the march's liqR for the Sun and Moon). The beams' light on a shaft
        // (beamFinish) took the rain phase whatever the temperature: a "satbow" in Antarctic snow.
        const double tShaft = tC + 0.0065 * (eyeAsl - (double)ground - 500.0);
        const double xl = std::clamp((tShaft + 1.0) / 4.0, 0.0, 1.0);
        p.precip = glm::vec4((float)tC, (float)std::fmod(simSecInDay, 600.0), std::clamp(cv2SnowWind, 0.0f, 8.0f),
                             (float)(xl * xl * (3.0 - 2.0 * xl)));
        cv2EyeTempC = (float)tC;
        cv2ShaftTempC = (float)tShaft;
        // The beams' light at the eye (SatelliteSim.cpp, from the cloud-light list) for the rain / snow particles.
        const float sunUpE = glm::dot(glm::vec3(p.sunE), glm::vec3(up));
        p.beamEye = glm::vec4(cv2BeamEyeIrr * std::max(cv2BeamLight, 0.0f),
                              1.0f - std::clamp((sunUpE + 0.2f) / 0.5f, 0.0f, 1.0f), 0.0f, 0.0f);
        p.beamEyeDir = glm::vec4(cv2BeamEyeDir, 0.0f);
        updateRainMotion(p);   // the rain particles' fall and wind (SatelliteSimRain.cpp)
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
        // The eye's own motion as PARALLAX (full-res moving mode 2): its step over the nearest a cloud can be (from orbit
        // the height above the shell, else 20 km) in half-res pixels, + history loss and time warp. A turn or a zoom
        // reprojects exactly and keeps the sparse grid's history (full rate there blurred a 165 deg/s pan at 455 km,
        // issue 20261022_220214 shot 3).
        const double nearM = std::max(eyeH - (double)hi, 0.0) + 20000.0;
        const bool eyeMoving = !valid || glm::length(eye - cv2PrevEye) / nearM > 0.25 * pixAng ||
                               std::abs(simT - cv2PrevSimT) > 0.5;
        cv2EyeStillFrames = eyeMoving ? 0 : std::min(cv2EyeStillFrames + 1, 1000);
    }
    cv2FullRateNow = eyeH > (double)cv2FullRateAboveKm * 1000.0 &&
                     !(cv2SparseWhenStill > 0.5f && cv2StillFrames >= 8);
    // S9: full-res clouds march sparse always: 1 pixel in 4 of the full-res grid is the ray count of the half-res full
    // rate, which is what the rates below exist to buy. full_res_moving_mode 2 (an A/B, not the default) marches every
    // pixel where the half-res path would march at full rate (the EYE moving from altitude).
    if (cv2FullResActive())
        cv2FullRateNow = cv2FullResMovingMode > 1.5f && eyeH > (double)cv2FullRateAboveKm * 1000.0 && cv2EyeStillFrames < 8;
    // "Half rate while moving": where it would run at full rate the march takes a checkerboard of the
    // half-res pixels, alternating each frame (every stale pixel's four neighbours are fresh: the
    // resolve's estimate for it), at about half the cost.
    // (Never on the full-res grid: neither the checkerboard nor the adaptive tiles were built for it — the tiles' full-rate
    // targets are half-res sized, and a pan at 455 km drew a stair-stepped frame of misplaced cloud, issue 20261022_220214
    // shot 3, once full_res_moving_mode 2 let the full-res grid reach full rate.)
    cv2HalfRateNow = cv2FullRateNow && cv2HalfRateMoving > 0.5f && !cv2FullResActive();
    // The adaptive rate ("dynamic sampling"): where the march would run at full rate because the view
    // moves, it runs the sparse grid (pass A) and only the tiles whose clouds show parallax, or whose
    // history comes from off screen, at full rate (pass B). A pan reprojects exactly: only the uncovered
    // edge goes full. Not with no history or in fast flight (> 150 m a frame: everything has parallax).
    {
        const uint32_t tiles = ((cv2HalfW + 31) / 32) * ((cv2HalfH + 31) / 32);
        cv2AdaptiveNow = cv2FullRateNow && !cv2HalfRateNow && cv2AdaptiveRate > 0.5f && valid && !cv2FullResActive() &&
                         glm::length(eye - cv2PrevEye) <= 150.0 && cv2MarchWg[0] == 16 && cv2MarchWg[1] == 16 &&
                         tiles <= kCv2MaxTiles && cv2MarchPassBPipeline != VK_NULL_HANDLE && cv2TilesPipeline != VK_NULL_HANDLE;
        if (cv2AdaptiveNow)
            cv2FullRateNow = false;
    }
    p.misc2.w = std::max(cv2AdaptiveParallaxPx, 0.05f);
    {
        const double move = glm::length(eye - cv2PrevEye);
        // 2 = whenever the eye moves (> 2 m a frame), boost or not (and how the harness, which holds no keys, tests it).
        cv2FastLodNow = cv2FastFlightLod > 0.5f && valid &&
                        (((boostHeldNow || cv2FastFlightLod > 1.5f) && move > 2.0) || move > 150.0);
        if (cv2FastLodNow)
        {
            p.light.y = std::min(p.light.y, 2.0f);
            p.march.x *= 1.5f;
        }
    }
    p.misc = glm::vec4(std::round(std::clamp(cv2MaxIters * (cv2FastLodNow ? 0.6f : 1.0f), 32.0f, 1024.0f)), cv2MoonGain,
                       cv2AdaptiveNow ? 3.0f : cv2HalfRateNow ? 2.0f : (cv2FullRateNow ? 1.0f : 0.0f), cv2MidAmount);
    // S9: the cloud grid (the march no longer takes it from the scene depth's size, which stays half res).
    // anvil3.zw (2026-10-23, the perf pass): an upper bound of the low field's top (cv2FieldLow's topMax, before the
    // storm cumulus target and the ground's lift) over every weather type, at the tropopause factor's two ends (0.8 at
    // the poles, 1.35 in the tropics; cv2Tropo). The bound per type mix is linear in the factor (the cumulus cap
    // min(T, cuT) taken as cuT), so the maximum over types is convex in it and lies under the chord between the ends:
    // the shader interpolates. + 100 m for the sampling of the type axis. Lets samples in the cirrus / anvil band skip
    // the low field's reads (an early out), with the same result.
    float lowTopB[2] = {0.0f, 0.0f};
    {
        const float trE[2] = {0.8f, 1.35f};
        auto sstep = [](float a, float b, float x) { float t = std::clamp((x - a) / (b - a), 0.0f, 1.0f); return t * t * (3.0f - 2.0f * t); };
        for (int e = 0; e < 2; ++e)
        {
            const float tr = trE[e];
            const GpuCloudV2Type &cu = cv2Types[2];
            const float cuT = cu.alt.x + (cu.alt.y - cu.alt.x) * tr;
            float m = 0.0f;
            for (int k = 0; k <= 4096; ++k)
            {
                const float g = (float)k / 4096.0f;
                const float fi = g * (float)(kCloudV2Types - 1);
                const int i0 = std::min((int)fi, kCloudV2Types - 2);
                const float fr = fi - (float)i0;
                const glm::vec4 alt = glm::mix(cv2Types[i0].alt, cv2Types[i0 + 1].alt, fr);
                const float lookZ = glm::mix(cv2Types[i0].look.z, cv2Types[i0 + 1].look.z, fr);
                const float T = alt.x + (alt.y - alt.x) * tr + std::max(1.0f - lookZ, 0.0f) * 3500.0f;
                const float wMap = sstep(0.8f, 0.95f, g);
                m = std::max(m, T * (1.0f - wMap) + wMap * std::max(cuT, 0.0f));
            }
            lowTopB[e] = m + 100.0f;
        }
    }
    p.anvil3 = glm::vec4(std::clamp(cv2AnvilPuff, 0.0f, 2.0f), std::clamp(cv2AnvilLiftM, 0.0f, 3000.0f), lowTopB[0], lowTopB[1]);
    p.perf = glm::vec4(std::clamp(std::round(cv2NearCirrusStride), 1.0f, 4.0f), 12.0f,
                       std::clamp(std::round(cv2GodrayReads), 2.0f, 8.0f), 0.0f);   // "Near cirrus stride", "God ray reads"
    p.grid = glm::vec4((float)cv2HalfW, (float)cv2HalfH, cv2FullResActive() ? 1.0f : 0.0f, cv2FullResActive() ? 1.0f : 0.0f);
    // Full-res clouds in motion from altitude (full_res_moving_mode; 2026-10-22, issues 20261022_210358 / _220214): 0 the
    // sparse rotation (default), 1 the sample held at one pixel of each 2x2 block, 2 full rate (above). Modes 1-2 were the
    // workaround for flicker and lost far cloud masses in motion whose cause was the march's half-res scene depth (one row
    // parity of rays clipped short near the limb, fixed in cloud_v2_march.comp); since that fix mode 0 matches mode 2
    // (flicker 0.79 vs 0.93%, far band within ~1 level of settled) at 1-6 ms less (harness_runs/rc1022g). Kept for A/B.
    cv2FullResHeldNow = cv2FullResActive() && cv2FullResMovingMode > 0.5f && cv2FullResMovingMode < 1.5f && !cv2FullRateNow && eyeH > (double)cv2FullRateAboveKm * 1000.0 &&
                        cv2StillFrames < 8;
    if (cv2FullResHeldNow)
        p.frame.z = p.frame.w = 0.0f;
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
        cv2FieldHash = hField;
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
    // The Sun three hours ago, Earth-fixed (the Earth rotation angle;
    // 3 h back it stood 45 deg further east), then into the map's (drifted) frame as cv2Drift turns it.
    const double g = earthRotationAngle(t) - 3.0 * 3600.0 * satphot::kOmegaEarth;
    const glm::dvec3 si = glm::dvec3(sunDirECI);
    const glm::dvec3 se(si.x * std::cos(g) + si.y * std::sin(g), -si.x * std::sin(g) + si.y * std::cos(g), si.z);
    const double dr = cloudDriftPhase();
    const glm::dvec3 sm(se.x * std::cos(dr) - se.y * std::sin(dr), se.x * std::sin(dr) + se.y * std::cos(dr), se.z);
    pc.evo2 = glm::vec4(glm::vec3(glm::normalize(sm)), std::max(cv2EvoDiurnal, 0.0f));
    // The drift itself: the land / sea mask is Earth-fixed, the map is not (cloud_v2_weather.comp).
    pc.wx = glm::vec4((float)std::cos(dr), (float)std::sin(dr), 0.0f, 0.0f);
    pc.remap = glm::vec4(cv2Coverage, cv2CoverClear, std::max(cv2CoverFull, cv2CoverClear + 0.01f), (float)mip);
    // The storms' own cloud (cloud_v2_weather.comp stormStamp): the tower geometry cv2ColumnSigma draws with.
    pc.storm = glm::vec4(std::clamp(cv2CbRadiusKm, 1.0f, 20.0f) * 1000.0f, cv2CbCumulusReachKm * 1000.0f,
                         std::clamp(cv2CbHeadDriftKm, 0.0f, 30.0f) * 1000.0f, std::clamp(cv2CbFlare, 0.5f, 3.0f));
    pc.storm2 = glm::vec4(stormObjects && cv2CbColumns > 0.0f ? std::max(stormCloudField, 0.0f) : 0.0f,
                          std::max(stormMoat, 0.0f), 0.0f, 0.0f);
    return pc;
}

uint64_t SatelliteSim::weatherBakeHash() const
{
    uint64_t h = 1469598103934665603ull;
    for (float v : {cv2EvoWindMps, cv2EvoGrowth, cv2EvoWindowH, cv2EvoDiurnal,
                    cv2Coverage, cv2CoverClear, cv2CoverFull,   // the coarse levels' coverage (review 18)
                    stormObjects ? 1.0f : 0.0f, stormCloudField, stormMoat, cv2CbColumns,   // the storms' stamp
                    cv2CbRadiusKm, cv2CbCumulusReachKm, cv2CbHeadDriftKm, cv2CbFlare})
        h = (h ^ std::hash<float>{}(v)) * 1099511628211ull;
    return h;
}

// One face of the weather cube per frame, all its mips, re-baked when sim time has moved on enough to
// show (the evolution at 1x is metres per frame; in time warp every frame) or the settings changed.
// The face is taken out of SHADER_READ_ONLY for the writes and returned before the passes read it.
void SatelliteSim::recordWeatherEvolution(VkCommandBuffer cmd)
{
    FPROF_SCOPE("recordWeatherEvolution");
    if (!cv2WxPipeline || cv2WxSets.size() != kCv2WeatherMips)
        return;
    const double t = (double)simDayJ2000 * 86400.0 + simSecInDay;
    const uint64_t h = weatherBakeHash();
    const bool evolving = cv2EvoWindMps > 0.0f || cv2EvoGrowth > 0.0f || cv2EvoDiurnal > 0.0f;
    // The storms' stamp (stormStamp) follows the towers' lifecycles (tens of minutes): a face at most ~45 s behind.
    const bool stormsStamped = stormObjects && cv2CbColumns > 0.0f && stormCloudField > 0.0f;
    if (h != cv2WxHash)
    {
        for (double &b : cv2WxBakedT)
            b = -1e30;   // every face stale: re-bake with the new settings (off: back to the static map)
        cv2WxHash = h;
    }
    // A face is due once it is this much sim time behind (a few km of wind drift at most).
    double due = evolving ? std::clamp(300.0 / std::max((double)cv2EvoWindMps, 1.0), 5.0, 120.0) : 1e30;
    if (stormsStamped)
        due = std::min(due, 45.0);
    int face = -1;
    double worst = 0.0;
    int nDue = 0;
    for (int f = 0; f < 6; ++f)
    {
        const double lag = std::fabs(t - cv2WxBakedT[f]);
        if (cv2WxBakedT[f] < -1e29 || lag > due)
            ++nDue;
        if ((cv2WxBakedT[f] < -1e29 || lag > due) && lag > worst)
        {
            worst = lag;
            face = f;
        }
    }
    // Time warp (2026-10-23, issue 20261023_224032 "Weather system updates choppy"): one face a frame covers the cube in
    // six frames, and once six frames of sim time pass `due` each face's storms jumped by six frames at a time, every face
    // at its own moment (from orbit: storm clouds stepping, a face at a time). Past a 15-min jump updateStorms makes every
    // face stale each frame, and the worst-first pick then took face 0 every frame: the other five froze until the warp
    // stopped. Then ALL SIX faces are baked together, every frame they are due, at one sim time (z = the faces).
    const double frameDt = std::fabs(t - cv2WxPrevT);
    cv2WxPrevT = t;
    const bool allSix = face >= 0 && (nDue == 6 || 6.0 * frameDt > due) && !cv2WxOneFace;
    if (face < 0)
        return;
    if (stormsStamped)
        buildStormEnvIndex();   // the storms' cloud field: the towers as they are now, where the cube is read for them
    if (allSix)
    {
        for (double &b : cv2WxBakedT)
            b = t;
        cv2WxRebakes += 6;
        ++cv2WxAllSixBakes;
        face = -1;
    }
    else
    {
        cv2WxBakedT[face] = t;
        ++cv2WxRebakes;
    }

    auto faceBarrier = [&](VkImageLayout from, VkImageLayout to, VkAccessFlags sa, VkAccessFlags da) {
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        b.oldLayout = from;
        b.newLayout = to;
        b.srcAccessMask = sa;
        b.dstAccessMask = da;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = cv2WeatherImg;
        // ALL six faces: the storage views (cv2WxSets) span the whole cube, and a descriptor's layout must
        // hold for every subresource it can reach — with only the baked face in GENERAL the validation layer
        // flagged every rebake (2026-09-30). The other faces are not written; the transition is ~free.
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, kCv2WeatherMips, 0, 6};
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
        vkCmdDispatch(cmd, g, g, face < 0 ? 6u : 1u);
    }
    faceBarrier(VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_SHADER_WRITE_BIT,
                VK_ACCESS_SHADER_READ_BIT);
}

void SatelliteSim::recordCloudsV2(VkCommandBuffer cmd, VulkanContext &ctx, const CloudMarchPC &cpc)
{
    FPROF_SCOPE("recordCloudsV2");
    // The field-evaluation counters (cv2EvalCount, -DCV2_EVAL_COUNT shader variants): last frame's totals were read by
    // the harness in buildUI; this frame's passes (lightning, march, cloud_march.comp) add from zero.
    if (cv2FlashMapped)
        std::memset(static_cast<char *>(cv2FlashMapped) + kCv2EvalCountOffset, 0, 32 * 4);
    // Last frame's pass times (its fence has been waited on), then this frame's stamps.
    if (cv2PassQuery && cv2PassStampsWritten)
    {
        uint64_t ts[kCv2PassStamps];
        if (vkGetQueryPoolResults(ctx.device, cv2PassQuery, 0, kCv2PassStamps, sizeof(ts), ts, sizeof(uint64_t),
                                  VK_QUERY_RESULT_64_BIT) == VK_SUCCESS)
            for (uint32_t i = 0; i + 1 < kCv2PassStamps; ++i) {
                cv2PassMsRaw[i] = (float)((double)(ts[i + 1] - ts[i]) * ctx.timestampPeriodNs * 1e-6);
                cv2PassMsEased[i] += (cv2PassMsRaw[i] - cv2PassMsEased[i]) * 0.033f;
            }
    }
    cv2PassStampsWritten = false;
    cv2PassStampNext = kCv2PassStamps;   // nothing is stamped unless the volumetric path runs (below)
    if (!cv2PassQuery && ctx.timestampPeriodNs > 0.0)
    {
        VkQueryPoolCreateInfo qi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        qi.queryType = VK_QUERY_TYPE_TIMESTAMP;
        qi.queryCount = kCv2PassStamps;
        if (vkCreateQueryPool(ctx.device, &qi, nullptr, &cv2PassQuery) != VK_SUCCESS)
            cv2PassQuery = VK_NULL_HANDLE;
    }
    fillCloudsV2Params(ctx, cpc);   // cloud_march.comp reads the v2 UBO (beams, debug views) either way
    updateStorms();                 // the storm objects' towers at this sim time (march, lightning, beams, clipmap)
    weatherFillDustGrid();          // the haboobs' dust walls round the observer (the march draws them as cloud)
    // The weather cube keeps evolving with the clouds off: the flat stand-ins (sat_sky.frag's flat layers,
    // SKY_LITE, Potato's deck, reflections and probes) read it.
    recordWeatherEvolution(cmd);
    if (!cv2MarchPipeline)
        return;
    // Knockout 32768 (Planetarium, Potato): nothing else of clouds v2 is recorded — no march, resolve, far layer,
    // lightning (so no bolts, rain particles, thunder or rain sound: they key on cv2LightningRanThisFrame /
    // cloudsV2LightningActive()), no tiles, and no barriers (each one restarts the compute encoder on MoltenVK).
    // cloud_march.comp ignores the stale resolved images under the same bit, and the history restarts after.
    if (cloudsV2Off())
    {
        cv2HistoryValid = false;
        cv2ScreenResetOnly = false;   // the Sun shadow grids are not baked meanwhile
        return;
    }
    if (cv2PassQuery)
    {
        vkCmdResetQueryPool(cmd, cv2PassQuery, 0, kCv2PassStamps);
        cv2PassStampNext = 0;
        cv2PassStampTo(cmd, 0);   // start
    }

    // scene_depth.comp wrote terrainFrameBuf (the eye height) this frame.
    memoryBarrier(cmd, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                  VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);

    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, cv2MarchPipeLayout, 0, 1, &cv2MarchDescSet, 0, nullptr);
    vkCmdPushConstants(cmd, cv2MarchPipeLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(cpc), &cpc);
    // The Sun / sky colour table: a function of the scattering gains alone, baked when one changes (and on the
    // first frame). The march, below, samples it.
    if (cv2SunLutPipeline &&
        (cv2SunLutKey[0] != cv2CloudSunRayleigh || cv2SunLutKey[1] != atmosRayleighGain || cv2SunLutKey[2] != atmosMieGain))
    {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, cv2SunLutPipeline);
        vkCmdDispatch(cmd, (kCv2SunLutW + 7) / 8, (kCv2SunLutH + 3) / 4, 1);
        memoryBarrier(cmd, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                      VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
        cv2SunLutKey[0] = cv2CloudSunRayleigh;
        cv2SunLutKey[1] = atmosRayleighGain;
        cv2SunLutKey[2] = atmosMieGain;
    }
    // The far cloud layer (review 18): full resolution, read by sat_sky.frag (imageLoad) this frame, after
    // the previous frame's fragment reads of it.
    const float farBlend = cloudFarBlend();
    if (cv2FarPipeline && farBlend > 0.0f)
    {
        memoryBarrier(cmd, VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_SHADER_WRITE_BIT,
                      VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, cv2FarPipeline);
        vkCmdDispatch(cmd, (ctx.swapExtent.width + 15) / 16, (ctx.swapExtent.height + 15) / 16, 1);
        memoryBarrier(cmd, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                      VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    }
    cv2PassStampTo(cmd, 1);   // Sun LUT + far layer
    // At full blend the layer is the clouds: no march, no resolve (cloud_march.comp fades their output out by
    // farBlend, so the stale resolved image is never seen). The history restarts on the way back down.
    if (farBlend >= 0.999f)
    {
        cv2PassStampTo(cmd, 7);
        cv2HistoryValid = false;
        cv2ScreenResetOnly = false;   // the Sun shadow grids are not baked meanwhile
        return;
    }
    // This frame's lightning flashes (one workgroup): cloud_march.comp draws them after the resolve.
    if (cv2LightningPipeline)
    {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, cv2LightningPipeline);
        // 0 the flash list, 1-4 the rain map, then the Sun transmittance volume (S3: logical workgroups 5-6 level 0, 7-9
        // levels 1-3, 10-17 catch-up), each split over kCv2StvG = 8 physical workgroups of 32 rays x 8 threads
        // (cloud_v2_lightning.comp, 2026-10-23: keep the two in step).
        constexpr uint32_t kCv2StvG = 8;
        vkCmdDispatch(cmd, 5 + 13 * kCv2StvG, 1, 1);
        // The rain particles (main pass, vertex stage) read the rain map and the light at the eye it just wrote.
        memoryBarrier(cmd, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                      VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_VERTEX_SHADER_BIT);
        cv2LightningRanThisFrame = true;
        tornadoProbeRan = true;   // (its threads 2-5 answered the tornado columns asked for: weatherFillTornadoes)
        cv2LightningObsDir = glm::dvec3(cpc.obsECEFDir);   // the frame its flash list is in (updateLightningBolts)
        cv2LightningObsDirValid = true;
    }
    // The terrain Sun-horizon map (2026-10-22): one thread per line, 4 x 128 x 128; after the lightning pass (it reads the
    // volume's axes, cv2SunProf), before the froxels, the sky pass and the fog march, which read it.
    if (cv2TshPipeline && cv2TshOnNow)
    {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, cv2TshPipeline);
        vkCmdDispatch(cmd, 4 * 128 * 128 / 64, 1, 1);
        memoryBarrier(cmd, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                      VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                      VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    }
    // The god rays' froxel volume (EXPERIMENT): from this frame's Sun shadow bakes, for the sky pass (fragment).
    if (cv2FroxPipeline && cv2GodrayFroxels > 0.5f)
    {
        memoryBarrier(cmd, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                      VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, cv2FroxPipeline);
        vkCmdDispatch(cmd, (kCv2FroxX + 7) / 8, (kCv2FroxY + 7) / 8, kCv2FroxZ / 4);
        memoryBarrier(cmd, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                      VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    }
    cv2PassStampTo(cmd, 2);   // lightning + the Sun shadow bakes (+ the froxel volume)
    // The cloud density clipmap (EXPERIMENT, cv2_cdc.glsl): stale cells re-baked within the budget; the march reads it.
    if (cv2CdcPipeline && cv2CdcBakePipeline && cv2CdcActive)
    {
        vkCmdFillBuffer(cmd, cv2CdcBuf, 0, 4, 0);   // the list's count
        memoryBarrier(cmd, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
                      VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, cv2CdcPipeline);
        // Only the levels the march reads (cdcB.w, "cdc max level", 6 by default; cv2_cdc.glsl returns "unknown" past it):
        // the check walked all 8 (8.4M cells a frame, ~0.5-1 ms whatever the view) and baked stale cells of levels no
        // lookup ever reads (2026-10-24, perf). A slot's index is level-major, so the first levels' workgroups are theirs.
        const uint32_t cdcLevels = (uint32_t)std::clamp((int)cv2CdcMaxLevel, 0, 8);
        if (cdcLevels > 0)
            vkCmdDispatch(cmd, kCv2CdcCells / 8u * cdcLevels / 256u, 1, 1);
        memoryBarrier(cmd, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
                      VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, cv2CdcBakePipeline);
        vkCmdDispatch(cmd, kCv2CdcListCap * 4 / 256, 1, 1);   // 4 lanes a listed cell; past the count they return
        memoryBarrier(cmd, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                      VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    }
    cv2PassStampTo(cmd, 3);   // layer shadow map + clipmap bake
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
    cv2PassStampTo(cmd, 4);   // adaptive-rate tiles
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, cv2MarchPipeline);
    const uint32_t gw = cv2HalfRateNow ? (cv2HalfW + 1) / 2 : cv2FullRateNow ? cv2HalfW : cv2QuarterW;
    const uint32_t gh = cv2FullRateNow ? cv2HalfH : cv2QuarterH;
    vkCmdDispatch(cmd, (gw + cv2MarchWg[0] - 1) / cv2MarchWg[0], (gh + cv2MarchWg[1] - 1) / cv2MarchWg[1], 1);
    cv2PassStampTo(cmd, 5);   // march (pass A)
    if (cv2AdaptiveNow)
    {
        // Pass B: 4 workgroups (16x16) per listed 32x32 tile, dispatched from the args pass A counted.
        memoryBarrier(cmd, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_INDIRECT_COMMAND_READ_BIT,
                      VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                      VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, cv2MarchPassBPipeline);
        vkCmdDispatchIndirect(cmd, cv2TileBuf, 4);
    }
    cv2PassStampTo(cmd, 6);   // march pass B

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
    vkCmdCopyImage(cmd, cv2AgeImg, VK_IMAGE_LAYOUT_GENERAL, cv2AgeHistImg, VK_IMAGE_LAYOUT_GENERAL, 1, &region);
    memoryBarrier(cmd, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                  VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    cv2PassStampTo(cmd, 7);   // resolve + history copies
}

void SatelliteSim::cv2PassStampTo(VkCommandBuffer cmd, uint32_t id)
{
    if (!cv2PassQuery)
        return;
    for (; cv2PassStampNext <= id && cv2PassStampNext < kCv2PassStamps; ++cv2PassStampNext)
        vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, cv2PassQuery, cv2PassStampNext);
    if (cv2PassStampNext == kCv2PassStamps)
        cv2PassStampsWritten = true;
}

// ─── The droplets' Mie phase (2026-10-22, tools/make_mie_lut.py) ───────────────────────────────────────────────────
// assets/textures/cloud_mie.rgba16f: 'SLMI', NA, NR, r_eff[NR], g[NR][3], then NR rows of NA RGBA16F texels (4 pi p / 8).
// Missing: a 1x1 texel and no rows (the share is then forced to 0: Henyey-Greenstein as before).
void SatelliteSim::createCloudMie(VulkanContext &ctx)
{
    uint32_t na = 1, nr = 1;
    std::vector<uint16_t> data(4, 0);
    cv2MieReff.clear();
    {
        const char *path = "assets/textures/cloud_mie.rgba16f";
        Log::line(std::string("init: texture: ") + path);
        if (FILE *f = std::fopen(path, "rb"))
        {
            char magic[4] = {};
            uint32_t hdr[2] = {};
            if (std::fread(magic, 1, 4, f) == 4 && std::memcmp(magic, "SLMI", 4) == 0 && std::fread(hdr, 4, 2, f) == 2
                && hdr[0] >= 2 && hdr[0] <= 16384 && hdr[1] >= 1 && hdr[1] <= 256)
            {
                std::vector<float> reff(hdr[1]), g((size_t)hdr[1] * 3);
                std::vector<uint16_t> d((size_t)hdr[0] * hdr[1] * 2 * 4);   // the phase rows, then the structure rows
                if (std::fread(reff.data(), 4, reff.size(), f) == reff.size() && std::fread(g.data(), 4, g.size(), f) == g.size()
                    && std::fread(d.data(), 2, d.size(), f) == d.size())
                {
                    na = hdr[0];
                    nr = hdr[1] * 2;
                    data.swap(d);
                    cv2MieReff = reff;
                }
            }
            std::fclose(f);
        }
        if (cv2MieReff.empty())
            Log::line("init: cloud Mie table missing (tools/make_mie_lut.py): Henyey-Greenstein only");
    }
    const VkDeviceSize total = (VkDeviceSize)data.size() * 2;
    VkBuffer stageBuf;
    VkDeviceMemory stageMem;
    ctx.createBuffer(total, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, stageBuf, stageMem);
    {
        void *mapped;
        vkMapMemory(ctx.device, stageMem, 0, total, 0, &mapped);
        std::memcpy(mapped, data.data(), (size_t)total);
        vkUnmapMemory(ctx.device, stageMem);
    }
    ctx.createImage(na, nr, VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                    cv2MieImg, cv2MieMem, 1);
    VkCommandBuffer cmd = ctx.beginOneTimeCommands();
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = cv2MieImg;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {na, nr, 1};
    vkCmdCopyBufferToImage(cmd, stageBuf, cv2MieImg, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
    ctx.endOneTimeCommands(cmd);
    vkDestroyBuffer(ctx.device, stageBuf, nullptr);
    vkFreeMemory(ctx.device, stageMem, nullptr);
    cv2MieView = makeView(ctx.device, cv2MieImg, VK_IMAGE_VIEW_TYPE_2D, VK_FORMAT_R16G16B16A16_SFLOAT, 0, 1, 1);
}

// ─── Cloud morphology (review 17) ─────────────────────────────────────────────────────────────────
// assets/textures/cloud_morph.rgba8 (tools/make_cloud_morph.py): raw square RGBA8, its size from the file's
// length (no decoding at launch, docs/FREEZES.md); mips box-filtered here. Without the file a 1x1 mid-grey
// texel: every morphology is then 0 (no structure), so the far field falls back to the Perlin's share.
void SatelliteSim::createCloudMorph(VulkanContext &ctx)
{
    uint32_t n = 1;
    std::vector<unsigned char> data(4, 128);
    {
        const char *path = "assets/textures/cloud_morph.rgba8";
        Log::line(std::string("init: texture: ") + path);
        if (FILE *f = std::fopen(path, "rb"))
        {
            std::fseek(f, 0, SEEK_END);
            const long bytes = std::ftell(f);
            std::fseek(f, 0, SEEK_SET);
            const uint32_t side = (uint32_t)std::lround(std::sqrt((double)std::max(bytes, 0L) / 4.0));
            if (side >= 16 && side <= 8192 && (long)side * side * 4 == bytes)
            {
                std::vector<unsigned char> d((size_t)bytes);
                if (std::fread(d.data(), 1, d.size(), f) == d.size())
                {
                    data.swap(d);
                    n = side;
                }
            }
            std::fclose(f);
        }
        if (n == 1)
            Log::line("init: cloud morphology missing (tools/make_cloud_morph.py): neutral texel");
    }
    uint32_t mips = 1;
    while ((n >> mips) > 0)
        ++mips;
    std::vector<std::vector<unsigned char>> levels(mips);
    levels[0] = std::move(data);
    for (uint32_t m = 1; m < mips; ++m)
    {
        const uint32_t pn = n >> (m - 1), cn = std::max(1u, n >> m);
        levels[m].resize((size_t)cn * cn * 4);
        const unsigned char *src = levels[m - 1].data();
        for (uint32_t y = 0; y < cn; ++y)
            for (uint32_t x = 0; x < cn; ++x)
                for (uint32_t c = 0; c < 4; ++c)
                {
                    const uint32_t x0 = 2 * x, y0 = 2 * y, x1 = std::min(x0 + 1, pn - 1), y1 = std::min(y0 + 1, pn - 1);
                    const uint32_t s = src[(y0 * pn + x0) * 4 + c] + src[(y0 * pn + x1) * 4 + c] +
                                       src[(y1 * pn + x0) * 4 + c] + src[(y1 * pn + x1) * 4 + c];
                    levels[m][(y * cn + x) * 4 + c] = (unsigned char)((s + 2) / 4);
                }
    }
    VkDeviceSize total = 0;
    for (auto &lv : levels)
        total += lv.size();
    VkBuffer stageBuf;
    VkDeviceMemory stageMem;
    ctx.createBuffer(total, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, stageBuf, stageMem);
    std::vector<VkBufferImageCopy> regions(mips);
    {
        void *mapped;
        vkMapMemory(ctx.device, stageMem, 0, total, 0, &mapped);
        VkDeviceSize off = 0;
        for (uint32_t m = 0; m < mips; ++m)
        {
            std::memcpy((unsigned char *)mapped + off, levels[m].data(), levels[m].size());
            regions[m] = {};
            regions[m].bufferOffset = off;
            regions[m].imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, m, 0, 1};
            regions[m].imageExtent = {std::max(1u, n >> m), std::max(1u, n >> m), 1};
            off += levels[m].size();
        }
        vkUnmapMemory(ctx.device, stageMem);
    }
    ctx.createImage(n, n, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                    cv2MorphImg, cv2MorphMem, mips);
    VkCommandBuffer cmd = ctx.beginOneTimeCommands();
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = cv2MorphImg;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, mips, 0, 1};
    b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
    vkCmdCopyBufferToImage(cmd, stageBuf, cv2MorphImg, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, mips, regions.data());
    b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
    ctx.endOneTimeCommands(cmd);
    vkDestroyBuffer(ctx.device, stageBuf, nullptr);
    vkFreeMemory(ctx.device, stageMem, nullptr);
    cv2MorphView = makeView(ctx.device, cv2MorphImg, VK_IMAGE_VIEW_TYPE_2D, VK_FORMAT_R8G8B8A8_UNORM, 0, mips, 1);
}

// ─── S9: the full-resolution combine (2026-10-22, .plans/CLOUD_REBUILD_PLAN.md 4e) ─────────────────────────────────
// "Full-resolution clouds" (clouds_v2.full_res): the v2 march and resolve run on the full render grid
// (computeCloudExtent), cloud_march.comp composites at half res into cmHalf* (A, B and the composite's derivatives
// with respect to the cloud), and cloud_v2_combine.comp writes the final cloud targets A/B at full res. Off, the four
// half images are 1x1 and nothing here runs.
// Set: 0 CloudParams, 1-4 the half images (A, B, dB, dA), 5/6 the resolved colour / depth, 7/8 the final targets.
void SatelliteSim::createCloudCombine(VulkanContext &ctx)
{
    VkDevice dev = ctx.device;
    const VkDescriptorType UBO = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, TEX = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                           IMG = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    cv2CombineDescLayout = makeSetLayout(dev, {UBO, TEX, TEX, TEX, TEX, TEX, TEX, IMG, IMG});
    VkDescriptorPoolSize ps[3] = {{UBO, 1}, {TEX, 6}, {IMG, 2}};
    VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pi.maxSets = 1;
    pi.poolSizeCount = 3;
    pi.pPoolSizes = ps;
    vkCreateDescriptorPool(dev, &pi, nullptr, &cv2CombineDescPool);
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = cv2CombineDescPool;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &cv2CombineDescLayout;
    vkAllocateDescriptorSets(dev, &ai, &cv2CombineDescSet);
    VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &cv2CombineDescLayout;
    vkCreatePipelineLayout(dev, &pli, nullptr, &cv2CombinePipeLayout);
    cv2CombinePipeline = makeComputePipeline(ctx, "shaders/cloud_v2_combine.comp.spv", cv2CombinePipeLayout);
    VkDescriptorBufferInfo cp{cloudParamsBuf, 0, sizeof(GpuCloudParams)};
    VkWriteDescriptorSet w = bufferWrite(cv2CombineDescSet, 0, UBO, &cp);
    vkUpdateDescriptorSets(dev, 1, &w, 0, nullptr);
}

void SatelliteSim::createCloudCombineTargets(VulkanContext &ctx)
{
    VkDevice dev = ctx.device;
    const bool on = cv2FullResActive();
    const VkExtent2D he = on ? computeHalfExtent(ctx) : VkExtent2D{1, 1};
    VkCommandBuffer cmd = ctx.beginOneTimeCommands();
    for (int i = 0; i < 4; ++i)
    {
        ctx.createImage(he.width, he.height, VK_FORMAT_R16G16B16A16_SFLOAT,
                        VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, cmHalfImg[i], cmHalfMem[i]);
        cmHalfView[i] = makeView(dev, cmHalfImg[i], VK_IMAGE_VIEW_TYPE_2D, VK_FORMAT_R16G16B16A16_SFLOAT, 0, 1, 1);
        ctx.imageBarrier(cmd, cmHalfImg[i], 0, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
                         VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                         VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    }
    ctx.endOneTimeCommands(cmd);

    const VkDescriptorType TEX = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, IMG = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    VkDescriptorImageInfo halfIn[4], halfOut[4];
    for (int i = 0; i < 4; ++i)
    {
        halfIn[i] = {cloudMarchSampler, cmHalfView[i], VK_IMAGE_LAYOUT_GENERAL};
        halfOut[i] = {VK_NULL_HANDLE, cmHalfView[i], VK_IMAGE_LAYOUT_GENERAL};
    }
    VkDescriptorImageInfo res{cv2ClampSampler, cv2ResolvedView, VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorImageInfo resDepth{cv2ClampSampler, cv2ResolvedDepthView, VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorImageInfo finalA{VK_NULL_HANDLE, cloudMarchTargetAView, VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorImageInfo finalB{VK_NULL_HANDLE, cloudMarchTargetBView, VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet w[] = {
        imageWrite(cv2CombineDescSet, 1, TEX, &halfIn[0]),
        imageWrite(cv2CombineDescSet, 2, TEX, &halfIn[1]),
        imageWrite(cv2CombineDescSet, 3, TEX, &halfIn[2]),
        imageWrite(cv2CombineDescSet, 4, TEX, &halfIn[3]),
        imageWrite(cv2CombineDescSet, 5, TEX, &res),
        imageWrite(cv2CombineDescSet, 6, TEX, &resDepth),
        imageWrite(cv2CombineDescSet, 7, IMG, &finalA),
        imageWrite(cv2CombineDescSet, 8, IMG, &finalB),
        // cloud_march.comp: its outputs are the half images with full-res clouds on, else the final targets.
        imageWrite(cloudMarchDescSet, 5, IMG, on ? &halfOut[0] : &finalA),
        imageWrite(cloudMarchDescSet, 6, IMG, on ? &halfOut[1] : &finalB),
        imageWrite(cloudMarchDescSet, 24, IMG, &halfOut[2]),
        imageWrite(cloudMarchDescSet, 25, IMG, &halfOut[3])};
    vkUpdateDescriptorSets(dev, (uint32_t)(sizeof(w) / sizeof(w[0])), w, 0, nullptr);
}

void SatelliteSim::destroyCloudCombineTargets(VkDevice device)
{
    for (int i = 0; i < 4; ++i)
    {
        if (cmHalfView[i]) vkDestroyImageView(device, cmHalfView[i], nullptr);
        if (cmHalfImg[i]) vkDestroyImage(device, cmHalfImg[i], nullptr);
        if (cmHalfMem[i]) vkFreeMemory(device, cmHalfMem[i], nullptr);
        cmHalfView[i] = VK_NULL_HANDLE;
        cmHalfImg[i] = VK_NULL_HANDLE;
        cmHalfMem[i] = VK_NULL_HANDLE;
    }
}

void SatelliteSim::destroyCloudCombine(VkDevice device)
{
    if (cv2CombinePipeline) vkDestroyPipeline(device, cv2CombinePipeline, nullptr);
    if (cv2CombinePipeLayout) vkDestroyPipelineLayout(device, cv2CombinePipeLayout, nullptr);
    if (cv2CombineDescPool) vkDestroyDescriptorPool(device, cv2CombineDescPool, nullptr);
    if (cv2CombineDescLayout) vkDestroyDescriptorSetLayout(device, cv2CombineDescLayout, nullptr);
    cv2CombinePipeline = VK_NULL_HANDLE;
    cv2CombinePipeLayout = VK_NULL_HANDLE;
    cv2CombineDescPool = VK_NULL_HANDLE;
    cv2CombineDescLayout = VK_NULL_HANDLE;
    cv2CombineDescSet = VK_NULL_HANDLE;
}

// After cloud_march.comp, with the final targets in GENERAL (recordCompute's barriers around it).
void SatelliteSim::recordCloudCombine(VkCommandBuffer cmd, VulkanContext &ctx)
{
    if (!cv2FullResActive() || !cv2CombinePipeline || computeCloudExtentBuilt.width != computeHalfExtentBuilt.width * 2)
        return;
    FPROF_SCOPE("cv2.combine");
    memoryBarrier(cmd, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                  VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, cv2CombinePipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, cv2CombinePipeLayout, 0, 1, &cv2CombineDescSet, 0, nullptr);
    vkCmdDispatch(cmd, (computeCloudExtentBuilt.width + 15) / 16, (computeCloudExtentBuilt.height + 15) / 16, 1);
    (void)ctx;
}

bool SatelliteSim::landWithin(double latRad, double lonRad, double radiusM) const
{
    if (oceanMaskCpu.empty() || oceanMaskW <= 0 || oceanMaskH <= 0)
        return true;
    const double pi = glm::pi<double>();
    const double dLat = radiusM / 6371000.0;
    const double lat0 = std::max(latRad - dLat, -0.5 * pi), lat1 = std::min(latRad + dLat, 0.5 * pi);
    const int y0 = std::clamp((int)std::floor((0.5 * pi - lat1) / pi * oceanMaskH), 0, oceanMaskH - 1);
    const int y1 = std::clamp((int)std::floor((0.5 * pi - lat0) / pi * oceanMaskH), 0, oceanMaskH - 1);
    // The longitude half-span at the box's most poleward latitude (the whole circle if it reaches a pole).
    const double cMin = std::cos(std::max(std::abs(lat0), std::abs(lat1)));
    const double dLon = (cMin * 6371000.0 > radiusM) ? std::asin(std::min(1.0, radiusM / (cMin * 6371000.0))) * 1.2 : pi;
    const int span = (dLon >= pi) ? oceanMaskW : std::min(oceanMaskW, (int)std::ceil(2.0 * dLon / (2.0 * pi) * oceanMaskW) + 2);
    const int xc = (int)std::floor((lonRad + pi) / (2.0 * pi) * oceanMaskW);
    const int xs = (span >= oceanMaskW) ? 0 : xc - span / 2;
    // Every texel of [i0, i1) an ocean bit (1)?
    auto allOcean = [&](size_t i0, size_t i1) {
        while (i0 < i1 && (i0 & 63))
        {
            if (!((oceanMaskCpu[i0 >> 6] >> (i0 & 63)) & 1ull))
                return false;
            ++i0;
        }
        for (; i0 + 64 <= i1; i0 += 64)
            if (oceanMaskCpu[i0 >> 6] != ~0ull)
                return false;
        for (; i0 < i1; ++i0)
            if (!((oceanMaskCpu[i0 >> 6] >> (i0 & 63)) & 1ull))
                return false;
        return true;
    };
    for (int y = y0; y <= y1; ++y)
    {
        const size_t row = (size_t)y * (size_t)oceanMaskW;
        int a = ((xs % oceanMaskW) + oceanMaskW) % oceanMaskW;
        int n = std::min(span, oceanMaskW);
        while (n > 0)
        {
            const int run = std::min(n, oceanMaskW - a);
            if (!allOcean(row + (size_t)a, row + (size_t)(a + run)))
                return true;
            n -= run;
            a = 0;
        }
    }
    return false;
}

bool SatelliteSim::terrainInSunBox(const glm::dvec3 &up, const glm::dvec3 &s, const glm::dvec3 &ga, double acrossM,
                                   double backM, double aheadM, float minM) const
{
    if (earthElevCpu.empty() || earthElevCpuW <= 0 || earthElevCpuH <= 0)
        return true;
    const double pi = glm::pi<double>(), R = 6371000.0;
    // The texel value at or above which the CPU DEM reads minM (cpuTerrainHeightM's decode).
    const int vMin = (int)std::ceil((15.0 / 255.0 + minM / 8848.0) * 255.0);
    const double radiusM = std::sqrt(acrossM * acrossM + std::max(backM, aheadM) * std::max(backM, aheadM));
    const double latRad = std::asin(std::clamp(up.z, -1.0, 1.0)), lonRad = std::atan2(up.y, up.x);
    const double dLat = radiusM / R;
    const double lat0 = std::max(latRad - dLat, -0.5 * pi), lat1 = std::min(latRad + dLat, 0.5 * pi);
    const int y0 = std::clamp((int)std::floor((0.5 * pi - lat1) / pi * earthElevCpuH), 0, earthElevCpuH - 1);
    const int y1 = std::clamp((int)std::floor((0.5 * pi - lat0) / pi * earthElevCpuH), 0, earthElevCpuH - 1);
    const double cMin = std::cos(std::max(std::abs(lat0), std::abs(lat1)));
    const double dLon = (cMin * R > radiusM) ? std::asin(std::min(1.0, radiusM / (cMin * R))) * 1.2 : pi;
    const int span = (dLon >= pi) ? earthElevCpuW
                                  : std::min(earthElevCpuW, (int)std::ceil(2.0 * dLon / (2.0 * pi) * earthElevCpuW) + 2);
    const int xc = (int)std::floor((lonRad + pi) / (2.0 * pi) * earthElevCpuW);
    const int xs = (span >= earthElevCpuW) ? 0 : xc - span / 2;
    for (int y = y0; y <= y1; ++y)
    {
        const double lat = 0.5 * pi - (y + 0.5) / earthElevCpuH * pi;
        for (int k = 0; k < span; ++k)
        {
            const int x = (((xs + k) % earthElevCpuW) + earthElevCpuW) % earthElevCpuW;
            if ((int)earthElevCpu[(size_t)y * earthElevCpuW + x] < vMin)
                continue;
            const double lon = (x + 0.5) / earthElevCpuW * 2.0 * pi - pi;
            const glm::dvec3 d(std::cos(lat) * std::cos(lon), std::cos(lat) * std::sin(lon), std::sin(lat));
            // Chord offsets (the texel's own size, ~18 km, is inside the 60-km margins the caller adds).
            const glm::dvec3 rel = (d - up) * R;
            const double a = glm::dot(rel, ga), al = glm::dot(rel, s);
            if (std::abs(a) <= acrossM && al >= -backM && al <= aheadM && glm::dot(d, up) > 0.0)
                return true;
        }
    }
    return false;
}
