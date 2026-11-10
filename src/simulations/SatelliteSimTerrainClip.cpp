// The terrain height clipmap (2026-11-10): shaders/include/terrain_clip.glsl has the design.
//
// The procedural terrain detail (terrain_detail.glsl: eight value octaves + the erosion octaves) is baked into
// kTcLevels nested, toroidal, observer-centred textures (terrain_clip_bake.comp), and the terrain marches of
// scene_depth.comp and sat_sky.frag (their TD_CLIPMAP variants) read it instead of evaluating it at every step.
// This file owns the textures, the bake pass and the window bookkeeping; the readers bind tcSet as set 1.
#include "SatelliteSim.h"
#include "Log.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>

namespace
{
VkImageView tcMakeArrayView(VkDevice dev, VkImage img, VkFormat fmt, uint32_t layers)
{
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = img;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    vi.format = fmt;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, layers};
    VkImageView v = VK_NULL_HANDLE;
    if (vkCreateImageView(dev, &vi, nullptr, &v) != VK_SUCCESS)
        throw std::runtime_error("terrain clipmap: image view");
    return v;
}

void tcMakeArrayImage(VulkanContext &ctx, VkFormat fmt, uint32_t n, uint32_t layers, VkImage &img, VkDeviceMemory &mem)
{
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = fmt;
    ci.extent = {n, n, 1};
    ci.mipLevels = 1;
    ci.arrayLayers = layers;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(ctx.device, &ci, nullptr, &img) != VK_SUCCESS)
        throw std::runtime_error("terrain clipmap: image");
    VkMemoryRequirements mr;
    vkGetImageMemoryRequirements(ctx.device, img, &mr);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = mr.size;
    ai.memoryTypeIndex = ctx.findMemoryType(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (vkAllocateMemory(ctx.device, &ai, nullptr, &mem) != VK_SUCCESS)
        throw std::runtime_error("terrain clipmap: image memory");
    vkBindImageMemory(ctx.device, img, mem, 0);
}

void tcArrayBarrier(VkCommandBuffer cmd, VkImage img, uint32_t layers, VkAccessFlags src, VkAccessFlags dst,
                    VkImageLayout from, VkImageLayout to, VkPipelineStageFlags srcStage, VkPipelineStageFlags dstStage)
{
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask = src;
    b.dstAccessMask = dst;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, layers};
    vkCmdPipelineBarrier(cmd, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &b);
}

uint64_t tcMix(uint64_t h, float v)
{
    uint32_t u;
    std::memcpy(&u, &v, 4);
    h ^= (uint64_t)u + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
    return h;
}
} // namespace

void SatelliteSim::createTerrainClip(VulkanContext &ctx)
{
    VkDevice dev = ctx.device;
    // The readers' set adds two samplers to sat_sky.frag's sixteen, the guaranteed per-stage minimum (build.md, "Old /
    // low-end hardware floor"): on a device that offers no more, there is no clipmap and the procedural shaders run.
    VkPhysicalDeviceProperties pp{};
    vkGetPhysicalDeviceProperties(ctx.physicalDevice, &pp);
    tcSupported = pp.limits.maxPerStageDescriptorSamplers >= 18 && pp.limits.maxPerStageDescriptorSampledImages >= 18;
    Log::line(std::string("terrain clipmap: ") + (tcSupported ? "supported" : "not supported (16 samplers a stage)"));
    if (!tcSupported)
        return;

    tcMakeArrayImage(ctx, VK_FORMAT_R32_SFLOAT, kTcN, kTcLevels, tcImgA, tcMemA);
    tcMakeArrayImage(ctx, VK_FORMAT_R16G16B16A16_SFLOAT, kTcN, kTcLevels - kTcBFirst, tcImgB, tcMemB);
    tcViewA = tcMakeArrayView(dev, tcImgA, VK_FORMAT_R32_SFLOAT, kTcLevels);
    tcViewB = tcMakeArrayView(dev, tcImgB, VK_FORMAT_R16G16B16A16_SFLOAT, kTcLevels - kTcBFirst);
    {
        VkCommandBuffer cmd = ctx.beginOneTimeCommands();
        VkClearColorValue zero{};
        for (VkImage img : {tcImgA, tcImgB})
        {
            const uint32_t layers = (uint32_t)(img == tcImgA ? kTcLevels : kTcLevels - kTcBFirst);
            VkImageSubresourceRange rng{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, layers};
            tcArrayBarrier(cmd, img, layers, 0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                           VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
            vkCmdClearColorImage(cmd, img, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &rng);
        }
        ctx.endOneTimeCommands(cmd);
    }
    {
        VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        si.addressModeU = si.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        si.maxLod = 0.0f;
        si.magFilter = si.minFilter = VK_FILTER_NEAREST;   // A: gathered, the weights are the shader's
        vkCreateSampler(dev, &si, nullptr, &tcSamplerA);
        si.magFilter = si.minFilter = VK_FILTER_LINEAR;
        vkCreateSampler(dev, &si, nullptr, &tcSamplerB);
    }
    void *m = createMirrored(ctx, sizeof(GpuTerrainClip), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, tcUboBuf, tcUboMem, true,
                             "terrainClip");
    tcUboMapped = m;

    // The readers' set (set 1 of the scene-depth and sky pipeline layouts): 0 the UBO, 1 A, 2 B.
    {
        const VkShaderStageFlags st = VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        VkDescriptorSetLayoutBinding b[3] = {{0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, st, nullptr},
                                             {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, st, nullptr},
                                             {2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, st, nullptr}};
        VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        li.bindingCount = 3;
        li.pBindings = b;
        vkCreateDescriptorSetLayout(dev, &li, nullptr, &tcSetLayout);
    }
    // The bake's set: 0 / 1 the elevation and water-class maps, 2 CloudParams, 3 / 4 A and B (storage), 5 the UBO.
    {
        const VkShaderStageFlags st = VK_SHADER_STAGE_COMPUTE_BIT;
        VkDescriptorSetLayoutBinding b[6] = {{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, st, nullptr},
                                             {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, st, nullptr},
                                             {2, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, st, nullptr},
                                             {3, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, st, nullptr},
                                             {4, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, st, nullptr},
                                             {5, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, st, nullptr}};
        VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        li.bindingCount = 6;
        li.pBindings = b;
        vkCreateDescriptorSetLayout(dev, &li, nullptr, &tcBakeLayout);
    }
    {
        VkDescriptorPoolSize ps[3] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 3},
                                      {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 4},
                                      {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2}};
        VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        pi.maxSets = 2;
        pi.poolSizeCount = 3;
        pi.pPoolSizes = ps;
        vkCreateDescriptorPool(dev, &pi, nullptr, &tcPool);
        VkDescriptorSetLayout lays[2] = {tcSetLayout, tcBakeLayout};
        VkDescriptorSet sets[2] = {};
        VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        ai.descriptorPool = tcPool;
        ai.descriptorSetCount = 2;
        ai.pSetLayouts = lays;
        vkAllocateDescriptorSets(dev, &ai, sets);
        tcSet = sets[0];
        tcBakeSet = sets[1];
    }
    {
        VkDescriptorBufferInfo ubo{tcUboBuf, 0, sizeof(GpuTerrainClip)};
        VkDescriptorBufferInfo cpUbo{cloudParamsBuf, 0, sizeof(GpuCloudParams)};
        VkDescriptorImageInfo aS{tcSamplerA, tcViewA, VK_IMAGE_LAYOUT_GENERAL};
        VkDescriptorImageInfo bS{tcSamplerB, tcViewB, VK_IMAGE_LAYOUT_GENERAL};
        VkDescriptorImageInfo aI{VK_NULL_HANDLE, tcViewA, VK_IMAGE_LAYOUT_GENERAL};
        VkDescriptorImageInfo bI{VK_NULL_HANDLE, tcViewB, VK_IMAGE_LAYOUT_GENERAL};
        VkDescriptorImageInfo elev{earthElevSampler ? earthElevSampler : noiseSampler,
                                   earthElevView ? earthElevView : noiseTexView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkDescriptorImageInfo spec{earthSpecSampler ? earthSpecSampler : noiseSampler,
                                   earthSpecView ? earthSpecView : noiseTexView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        auto W = [](VkDescriptorSet s, uint32_t b, VkDescriptorType t, const VkDescriptorImageInfo *ii,
                    const VkDescriptorBufferInfo *bi)
        {
            VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            w.dstSet = s;
            w.dstBinding = b;
            w.descriptorCount = 1;
            w.descriptorType = t;
            w.pImageInfo = ii;
            w.pBufferInfo = bi;
            return w;
        };
        const VkDescriptorType U = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, T = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                               I = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        VkWriteDescriptorSet w[] = {W(tcSet, 0, U, nullptr, &ubo),       W(tcSet, 1, T, &aS, nullptr),
                                    W(tcSet, 2, T, &bS, nullptr),        W(tcBakeSet, 0, T, &elev, nullptr),
                                    W(tcBakeSet, 1, T, &spec, nullptr),  W(tcBakeSet, 2, U, nullptr, &cpUbo),
                                    W(tcBakeSet, 3, I, &aI, nullptr),    W(tcBakeSet, 4, I, &bI, nullptr),
                                    W(tcBakeSet, 5, U, nullptr, &ubo)};
        frameSafeUpdateDescriptorSets(dev, (uint32_t)(sizeof(w) / sizeof(w[0])), w, 0, nullptr);
    }
    {
        VkPushConstantRange pcr{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(TerrainClipBakePC)};
        VkPipelineLayoutCreateInfo li{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        li.setLayoutCount = 1;
        li.pSetLayouts = &tcBakeLayout;
        li.pushConstantRangeCount = 1;
        li.pPushConstantRanges = &pcr;
        vkCreatePipelineLayout(dev, &li, nullptr, &tcBakePipeLayout);
        VkShaderModule mod = ctx.loadShader("shaders/terrain_clip_bake.comp.spv");
        VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        ci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        ci.stage.module = mod;
        ci.stage.pName = "main";
        ci.layout = tcBakePipeLayout;
        const VkResult r = vkCreateComputePipelines(dev, ctx.pipelineCache, 1, &ci, nullptr, &tcBakePipeline);
        vkDestroyShaderModule(dev, mod, nullptr);
        if (r != VK_SUCCESS)
            throw std::runtime_error("terrain clipmap: bake pipeline");
    }
    tcAnchorValid = false;
}

void SatelliteSim::destroyTerrainClip(VulkanContext &ctx)
{
    VkDevice dev = ctx.device;
    if (tcBakePipeline) vkDestroyPipeline(dev, tcBakePipeline, nullptr);
    if (tcBakePipeLayout) vkDestroyPipelineLayout(dev, tcBakePipeLayout, nullptr);
    if (tcPool) vkDestroyDescriptorPool(dev, tcPool, nullptr);
    if (tcSetLayout) vkDestroyDescriptorSetLayout(dev, tcSetLayout, nullptr);
    if (tcBakeLayout) vkDestroyDescriptorSetLayout(dev, tcBakeLayout, nullptr);
    if (tcSamplerA) vkDestroySampler(dev, tcSamplerA, nullptr);
    if (tcSamplerB) vkDestroySampler(dev, tcSamplerB, nullptr);
    if (tcViewA) vkDestroyImageView(dev, tcViewA, nullptr);
    if (tcViewB) vkDestroyImageView(dev, tcViewB, nullptr);
    if (tcImgA) vkDestroyImage(dev, tcImgA, nullptr);
    if (tcImgB) vkDestroyImage(dev, tcImgB, nullptr);
    if (tcMemA) vkFreeMemory(dev, tcMemA, nullptr);
    if (tcMemB) vkFreeMemory(dev, tcMemB, nullptr);
    if (tcUboBuf) destroyMirrored(ctx, tcUboBuf, tcUboMem, tcUboMapped);
    tcBakePipeline = VK_NULL_HANDLE;
    tcBakePipeLayout = VK_NULL_HANDLE;
    tcPool = VK_NULL_HANDLE;
    tcSetLayout = tcBakeLayout = VK_NULL_HANDLE;
    tcSamplerA = tcSamplerB = VK_NULL_HANDLE;
    tcViewA = tcViewB = VK_NULL_HANDLE;
    tcImgA = tcImgB = VK_NULL_HANDLE;
    tcMemA = tcMemB = VK_NULL_HANDLE;
    tcSet = tcBakeSet = VK_NULL_HANDLE;
}

// Once a frame, before the scene-depth passes: the UBO for this frame's observer, and the bake of every texel that
// entered a level's window.
//
// A level's window is the kTcN x kTcN texels centred on the observer's cell; what was baked last frame and is still
// inside stays (a texel's content is a function of its absolute cell), so a moving observer bakes the two strips that
// came in. A level whose window moved by half its width or more in one frame is left unbaked (it would be rebaked
// whole every frame) and the readers start at the next level (tcAxU.w): fast flight near the ground.
// The finest level worth keeping comes from the eye's height above the ground: the geometry LOD is at least 1.2% of
// the distance, so from 5 km up nothing reads the 8-m level.
// Everything is rebaked when what the detail is made of changes (the terrain sliders, the launch pads' graded ground)
// and when the observer is 300 km from the anchor (the plane's foreshortening there is 0.1%).
void SatelliteSim::recordTerrainClip(VkCommandBuffer cmd, VulkanContext &ctx, const glm::vec3 &obsDirF, const GpuCloudParams &cp)
{
    tcBakedTexels = 0;
    if (!tcSupported || !tcUboMapped)
        return;
    const double R = 6371000.0;   // common.glsl R_EARTH
    const glm::dvec3 up = glm::normalize(glm::dvec3(obsDirF));

    uint64_t ep = 0x1234ull;
    for (float v : {cp.terrainDetailStrength, cp.terrainDetailAmpM, cp.terrainDetailGain, cp.terrainDetailErode,
                    cp.terrainErosion.x, cp.terrainErosion.y, cp.envMainObsDir.w, cp.padInfo.w})
        ep = tcMix(ep, v);
    for (int k = 0; k < (int)(cp.padInfo.w + 0.5f) && k < (int)(sizeof(cp.padAux) / sizeof(cp.padAux[0])); ++k)
    {
        ep = tcMix(ep, cp.padAux[k].x);
        ep = tcMix(ep, cp.padAux[k].y);
        ep = tcMix(ep, cp.padAux[k].z);
    }
    const bool reset = !tcAnchorValid || ep != tcEpoch || glm::dot(up, tcAxU) < std::cos(300000.0 / R);
    if (reset)
    {
        tcAxU = up;
        glm::dvec3 e = glm::cross(glm::dvec3(0.0, 0.0, 1.0), up);
        if (glm::length(e) < 1e-6)
            e = glm::dvec3(1.0, 0.0, 0.0);
        tcAxE = glm::normalize(e);
        tcAxN = glm::cross(tcAxU, tcAxE);
        tcAnchorValid = true;
        tcEpoch = ep;
        for (int j = 0; j < kTcLevels; ++j)
            tcWinValid[j] = false;
        tcPrevValid = false;
    }
    const double ox = R * glm::dot(up, tcAxE), oy = R * glm::dot(up, tcAxN), oz = R * glm::dot(up, tcAxU);
    const double moved = tcPrevValid ? std::max(std::abs(ox - tcPrevOx), std::abs(oy - tcPrevOy)) : 0.0;
    tcPrevOx = ox;
    tcPrevOy = oy;
    tcPrevValid = true;

    // The finest level worth baking: the geometry LOD is at least 1.2% of the distance, so nothing nearer than the
    // nearest ground reads a finer level. The nearest ground from the CPU's DEM: under the eye, and on five rings round
    // it (a quarter to four times the eye's height above the ground, eight bearings each): an eye 900 m over an Alpine
    // valley floor has the valley's wall 300 m away, and half the height under it (the first cut) left that wall on a
    // level without its finest octaves for the first frames of a climb. The detail's own relief (its octave-0
    // amplitude x 2) counts as ground, and 0.7 of the distance is taken.
    int finest = 0;
    {
        const double relief = 2.0 * (double)cp.terrainDetailAmpM * (double)cp.terrainDetailStrength;
        const double eyeH   = (double)std::max(obsTerrainH, obsHeightOffset) + 2.0;
        const double agl    = std::max(eyeH - ((double)obsTerrainH + relief), 0.0);
        double dMin = agl;
        if (agl > 0.0)
        {
            const double latR = glm::radians((double)obsLatDeg), cosLat = std::max(std::cos(latR), 0.02);
            const double base = std::max(agl, 100.0);
            for (int ring = 0; ring < 5 && dMin > 0.0; ++ring)
            {
                const double r = base * 0.25 * std::exp2((double)ring);
                for (int b8 = 0; b8 < 8; ++b8)
                {
                    const double az = (b8 + 0.5 * (ring & 1)) * (glm::pi<double>() / 4.0);
                    const double dLat = glm::degrees(r * std::cos(az) / R), dLon = glm::degrees(r * std::sin(az) / (R * cosLat));
                    const double h = (double)cpuTerrainHeightM((float)((double)obsLatDeg + dLat), (float)((double)obsLonDeg + dLon)) + relief;
                    dMin = std::min(dMin, h >= eyeH ? r : std::sqrt(r * r + (eyeH - h) * (eyeH - h)));
                }
            }
        }
        finest = std::clamp((int)std::floor(std::log2(std::max(0.012 * 0.7 * dMin / kTcLod0, 1.0))), 0, kTcLevels - 1);
    }

    GpuTerrainClip u{};
    int64_t cellX[kTcLevels], cellY[kTcLevels];
    for (int j = 0; j < kTcLevels; ++j)
    {
        const double texel = kTcLod0 * std::exp2((double)j) / 3.0;
        const double fx = std::floor(ox / texel), fy = std::floor(oy / texel);
        cellX[j] = (int64_t)fx;
        cellY[j] = (int64_t)fy;
        u.level[j] = glm::vec4((float)(((cellX[j] % kTcN) + kTcN) % kTcN), (float)(((cellY[j] % kTcN) + kTcN) % kTcN),
                               (float)(ox / texel - fx), (float)(oy / texel - fy));
        u.cell[j] = glm::ivec4((int)cellX[j], (int)cellY[j], 0, 0);
        // (A level the observer crosses half of in a frame is not kept.)
        if (moved / texel >= 0.5 * kTcN)
        {
            tcWinValid[j] = false;
            finest = std::max(finest, std::min(j + 1, kTcLevels - 1));
        }
    }
    // (No bake without the detail: the readers do not look, and switching it on changes the epoch.)
    const bool active = terrainClipActive() && cp.terrainDetailStrength > 0.0f;
    tcFinest = finest;
    u.axE = glm::vec4(glm::vec3(tcAxE), 0.0f);
    u.axN = glm::vec4(glm::vec3(tcAxN), 0.0f);
    u.axU = glm::vec4(glm::vec3(tcAxU), (float)finest);
    u.obs = glm::vec4((float)ox, (float)oy, (float)oz, 0.0f);
    std::memcpy(tcUboMapped, &u, sizeof(u));
    if (!active)
    {
        for (int j = 0; j < kTcLevels; ++j)
            tcWinValid[j] = false;
        return;
    }

    bool bound = false;
    TerrainClipBakePC pc{};
    pc.obsECEFDir = glm::vec4(glm::vec3(up), 0.0f);
    auto bake = [&](int j, int64_t x0, int64_t y0, int64_t w, int64_t h)
    {
        if (w <= 0 || h <= 0)
            return;
        if (!bound)
        {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, tcBakePipeline);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, tcBakePipeLayout, 0, 1, &tcBakeSet, 0, nullptr);
            bound = true;
        }
        pc.rect = glm::ivec4((int)x0, (int)y0, (int)w, (int)h);
        pc.lvl = glm::ivec4(j, 0, 0, 0);
        vkCmdPushConstants(cmd, tcBakePipeLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
        vkCmdDispatch(cmd, (uint32_t)((w + 7) / 8), (uint32_t)((h + 7) / 8), 1);
        tcBakedTexels += (uint32_t)(w * h);
    };
    for (int j = finest; j < kTcLevels; ++j)
    {
        const int64_t N = kTcN, x0 = cellX[j] - N / 2, y0 = cellY[j] - N / 2;
        const int64_t dx = x0 - tcWinX[j], dy = y0 - tcWinY[j];
        if (!tcWinValid[j] || std::abs(dx) >= N || std::abs(dy) >= N)
            bake(j, x0, y0, N, N);
        else
        {
            // The columns that came in, over the whole height; then the rows that came in, over the columns kept.
            if (dx > 0) bake(j, tcWinX[j] + N, y0, dx, N);
            if (dx < 0) bake(j, x0, y0, -dx, N);
            const int64_t kx0 = std::max(x0, tcWinX[j]), kw = N - std::abs(dx);
            if (dy > 0) bake(j, kx0, tcWinY[j] + N, kw, dy);
            if (dy < 0) bake(j, kx0, y0, kw, -dy);
        }
        tcWinValid[j] = true;
        tcWinX[j] = x0;
        tcWinY[j] = y0;
    }
    if (bound)
        for (VkImage img : {tcImgA, tcImgB})
            tcArrayBarrier(cmd, img, (uint32_t)(img == tcImgA ? kTcLevels : kTcLevels - kTcBFirst), VK_ACCESS_SHADER_WRITE_BIT,
                           VK_ACCESS_SHADER_READ_BIT,
                           VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                           VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
}
