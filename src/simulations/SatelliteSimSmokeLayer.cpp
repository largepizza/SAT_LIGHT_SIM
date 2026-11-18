// The wildfire smoke LAYER (2026-11-17, issue_20261117_195951 "Smoke trails or bust": "a greyer effect with more broad
// diffusion in a layer (see launch site noct cloud implementation). This layer needs to make sunsets redder, and be a
// visible haze on the horizon"). The smoke older than the streaks, each fire's mass-conserving PALL (weatherFillWildfires),
// was one flat Gaussian in the dust list; it is now a map of the layer's column, mid height and thickness in the eye's
// tangent plane (rebuilt each frame from the palls in reach, spread by clouds_v2.wildfire_smoke_layer_spread_km), which
// shaders/smoke_layer.comp integrates per ray in curved coordinates after the cloud composite, as the noctilucent layer is
// (SatelliteSimNLC.cpp). The palls also go to the dust buffer's tail as Gaussians NOT in the drawn list, so sat_sky.frag
// smokeSunTransmit still shades and reddens the ground's sunlight under them. docs/subsystems/weather.md "The smoke layer".

#include "SatelliteSim.h"
#include "SmokeLayerCpu.h"
#include "../VulkanContext.h"
#include "../FrameProfile.h"
#include "../Log.h"
#include <glm/gtc/packing.hpp>
#include <algorithm>
#include <chrono>
#include <cstring>

namespace
{
constexpr double kReL = 6371000.0;
constexpr double kPiL = 3.14159265358979323846;
// The layer's light (smoke_layer.comp SmokeLayerBuf.opt): aged smoke scatters most of what it intercepts (single-scattering
// albedo ~0.9 at 550 nm, AERONET over aged boreal / Californian smoke) with a moderate forward peak (HG g ~0.6); the sky's
// light and the smoke's own multiple scattering are a grey share of the Sun's, which keeps it grey away from the Sun.
constexpr float kLayerAlbedo = 0.9f;
constexpr float kLayerG = 0.6f;
constexpr float kLayerSkyShare = 0.35f;
// Its colour through it: "Smoke spectral" scales the Angstrom exponent, whose default 0.6 keeps the streaks from turning
// every cloud behind them tan; the layer takes x 1 / 0.6 of it, the physical ~1.6 at the default (aged smoke's 1.4-2):
// the Sun and the sunset seen through the haze redden as they do through real smoke.
constexpr float kLayerSpecK = 1.0f / 0.6f;

// The last map, for the CPU's lookups (smokeLayerSunTransmitCpu): its frame, half-size and texels
struct CpuMap
{
    bool on = false;
    glm::dvec3 E{1, 0, 0}, N{0, 1, 0}, U{0, 0, 1};
    double half = 1.0;
    int W = 0;
    std::vector<float> tau, zm, sz;
};
CpuMap gMap;

// bilinear (the heights by column), as smoke_layer.comp layerAt; x tau, y mid height, z sigma
glm::dvec3 cpuLayerAt(const glm::dvec3 &P)
{
    const CpuMap &M = gMap;
    const double pz = glm::dot(P, M.U);
    if (!M.on || pz <= 1.0) return glm::dvec3(0.0, 2000.0, 800.0);
    const double gx = glm::dot(P, M.E) * (kReL / pz), gy = glm::dot(P, M.N) * (kReL / pz);
    const double ux = (gx / M.half * 0.5 + 0.5) * M.W - 0.5, uy = (gy / M.half * 0.5 + 0.5) * M.W - 0.5;
    const int ix = (int)std::floor(ux), iy = (int)std::floor(uy);
    const double fx = ux - ix, fy = uy - iy;
    double t = 0.0, z = 0.0, s = 0.0, w = 1e-12;
    for (int k = 0; k < 4; ++k)
    {
        const int x = ix + (k & 1), y = iy + (k >> 1);
        if (x < 0 || y < 0 || x >= M.W || y >= M.W) continue;
        const double b = ((k & 1) ? fx : 1.0 - fx) * ((k >> 1) ? fy : 1.0 - fy);
        const size_t i = (size_t)y * M.W + x;
        const double bt = b * M.tau[i];
        t += bt;
        z += (bt + b * 1e-6) * M.zm[i];
        s += (bt + b * 1e-6) * M.sz[i];
        w += bt + b * 1e-6;
    }
    return glm::dvec3(t, z / w, std::max(s / w, 100.0));
}
}

glm::dvec3 smokeLayerSunTransmitCpu(const glm::dvec3 &P, const glm::dvec3 &sunE, double specS, double *colAbove)
{
    if (colAbove) *colAbove = 0.0;
    if (!gMap.on) return glm::dvec3(1.0);
    const double r = glm::length(P);
    const double h = r - kReL;
    const double mu = glm::dot(P, sunE) / std::max(r, 1.0);
    const glm::dvec3 lay = cpuLayerAt(P);
    if (!(lay.x > 1e-5)) return glm::dvec3(1.0);
    if (colAbove) *colAbove = lay.x * 0.5 * std::erfc((h - lay.y) / (1.41421356 * lay.z));
    // (as smoke_layer.comp: the column above the point, read where the Sun's ray reaches the layer's middle, along a
    // slab's slant capped at grazing)
    const double muE = std::max(mu, 0.0);
    const double dS = std::clamp((std::max(lay.y - h, 0.0) + lay.z) / std::max(muE, 0.03), 0.0, 400000.0);
    const glm::dvec3 layS = cpuLayerAt(P + sunE * dS);
    const double above = 0.5 * std::erfc((h - layS.y) / (1.41421356 * layS.z));
    const double tauS = layS.x * above / std::sqrt(muE * muE + 4.0 * layS.z / kReL);
    const glm::dvec3 spec(std::pow(0.765, specS), 1.0, std::pow(1.378, specS));   // (smoke.glsl kSmokeSpec ^ s)
    // + the light the haze scatters on into itself (smoke_layer.comp's two octaves, x the share its own column scatters):
    // the direct beam alone left a streak in the haze at a low Sun lit by the blue sky only, lavender
    const glm::dvec3 tS = tauS * spec;
    const double ms = 1.0 - std::exp(-lay.x);
    return glm::min(glm::exp(-tS) + ms * (0.5 * glm::exp(-0.25 * tS) + 0.25 * glm::exp(-0.0625 * tS)), glm::dvec3(1.0));
}

void SatelliteSim::createSmokeLayer(VulkanContext &ctx)
{
    const VkDevice dev = ctx.device;
    const VkDeviceSize sz = kSmokeLayerHeader + (VkDeviceSize)kSmokeLayerRes * kSmokeLayerRes * sizeof(uint32_t);
    smokeLayerMapped = createMirrored(ctx, sz, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, smokeLayerBuf, smokeLayerMem, false, "smokeLayer");
    if (smokeLayerMapped)
        std::memset(smokeLayerMapped, 0, (size_t)sz);
    const VkDescriptorType kTypes[4] = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                                        VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER};
    VkDescriptorSetLayoutBinding b[4];
    for (uint32_t i = 0; i < 4; ++i)
        b[i] = {i, kTypes[i], 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    li.bindingCount = 4;
    li.pBindings = b;
    vkCreateDescriptorSetLayout(dev, &li, nullptr, &smokeLayerDescLayout);
    VkDescriptorPoolSize ps[3] = {{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1},
                                  {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2},
                                  {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1}};
    VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pi.maxSets = 1;
    pi.poolSizeCount = 3;
    pi.pPoolSizes = ps;
    vkCreateDescriptorPool(dev, &pi, nullptr, &smokeLayerDescPool);
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = smokeLayerDescPool;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &smokeLayerDescLayout;
    vkAllocateDescriptorSets(dev, &ai, &smokeLayerDescSet);
    VkPushConstantRange pcr{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(CloudMarchPC)};
    VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &smokeLayerDescLayout;
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges = &pcr;
    vkCreatePipelineLayout(dev, &pli, nullptr, &smokeLayerPipeLayout);
    const std::string err = buildSmokeLayerPipeline(ctx, "");
    if (!err.empty())
        Log::line("init: WARNING smoke_layer pipeline: " + err + " (the wildfire smoke layer off)");
    writeSmokeLayerDescriptors(ctx);
}

std::string SatelliteSim::buildSmokeLayerPipeline(VulkanContext &ctx, const std::string &spv)
{
    VkShaderModule mod = VK_NULL_HANDLE;
    try
    {
        mod = ctx.loadShader(spv.empty() ? std::string("shaders/smoke_layer.comp.spv") : spv);
    }
    catch (const std::exception &ex)
    {
        return ex.what();
    }
    VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    ci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, mod, "main", nullptr};
    ci.layout = smokeLayerPipeLayout;
    VkPipeline p = VK_NULL_HANDLE;
    const VkResult r = vkCreateComputePipelines(ctx.device, ctx.pipelineCache, 1, &ci, nullptr, &p);
    vkDestroyShaderModule(ctx.device, mod, nullptr);
    if (r != VK_SUCCESS)
        return "pipeline creation failed";
    if (smokeLayerPipe != VK_NULL_HANDLE)
        vkDestroyPipeline(ctx.device, smokeLayerPipe, nullptr);
    smokeLayerPipe = p;
    return "";
}

void SatelliteSim::writeSmokeLayerDescriptors(VulkanContext &ctx)
{
    if (!smokeLayerDescSet || !cloudMarchTargetAView || !sceneDepthView)
        return;
    VkDescriptorBufferInfo bufI{smokeLayerBuf, 0, VK_WHOLE_SIZE};
    VkDescriptorImageInfo tA{VK_NULL_HANDLE, cloudMarchTargetAView, VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorImageInfo tB{VK_NULL_HANDLE, cloudMarchTargetBView, VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorImageInfo depth{sceneDepthSampler, sceneDepthView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    auto W = [&](uint32_t bind, VkDescriptorType t, const VkDescriptorImageInfo *ii, const VkDescriptorBufferInfo *bi) {
        return VkWriteDescriptorSet{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, smokeLayerDescSet, bind, 0, 1, t, ii, bi, nullptr};
    };
    VkWriteDescriptorSet w[4] = {W(0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &bufI),
                                 W(1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &tA, nullptr),
                                 W(2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &tB, nullptr),
                                 W(3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &depth, nullptr)};
    frameSafeUpdateDescriptorSets(ctx.device, 4, w, 0, nullptr);
}

// The map from this frame's palls (weatherFillWildfires fills smokePalls), in the eye's tangent plane (gnomonic: a point
// at ENU P maps to R P.xy / P.z, smoke_layer.comp layerAt), and the palls as shadow-only Gaussians in the dust buffer's tail.
void SatelliteSim::smokeLayerBuild(const glm::dvec3 &upIn)
{
    FPROF_SCOPE("smokeLayerBuild");
    const auto t0 = std::chrono::steady_clock::now();
    smokeLayerAny = false;
    smokeLayerMaxTau = 0.0f;
    const bool on = wildfireSmokeLayer > 0.0f && wildfireRate > 0.0f && !smokePalls.empty();
    const glm::dvec3 up = glm::normalize(upIn);
    const glm::dvec3 E = glm::normalize(glm::cross(glm::dvec3(0, 0, 1), up) + glm::dvec3(1e-12, 0, 0));
    const glm::dvec3 N = glm::cross(up, E);
    const double eyeR = (double)obsEyeRadiusM();

    // ── The shadow-only Gaussians (sat_sky.frag smokeSunTransmit): slot 0's c.x = the count, then one per pall ──
    if (kDustMapped)
    {
        GpuDustCloud *tail = reinterpret_cast<GpuDustCloud *>(static_cast<char *>(kDustMapped) + 16) +
                             (kMaxDustClouds + kMaxEngineLights + kDustSideSlots);
        uint32_t k = 0;
        if (on)
        {
            // (the largest cross-sections first: the palls and the trail's heaviest stretches)
            std::vector<uint32_t> ord(smokePalls.size());
            for (uint32_t i = 0; i < (uint32_t)ord.size(); ++i) ord[i] = i;
            // (at most kShadowUsed: sat_sky.frag walks them for every ground pixel; 31 of them cost the report's
            // shot 5 ~+0.6 ms of the sky pass)
            constexpr size_t kShadowUsed = 12;
            const size_t nSh = std::min<size_t>(ord.size(), std::min<size_t>(kSmokeShadowSlots - 1, kShadowUsed));
            std::partial_sort(ord.begin(), ord.begin() + nSh, ord.end(),
                              [&](uint32_t x, uint32_t y) { return smokePalls[x].Sigma > smokePalls[y].Sigma; });
            for (size_t oi = 0; oi < nSh; ++oi)
            {
                const SmokePall &P = smokePalls[ord[oi]];
                const glm::dvec3 C = P.c * (kReL + P.zM);
                const glm::dvec3 rel = C - up * eyeR;
                auto toEnu = [&](const glm::dvec3 &v) { return glm::dvec3(glm::dot(v, E), glm::dot(v, N), glm::dot(v, up)); };
                const glm::dvec3 ae = toEnu(P.a), be = toEnu(P.b), ce = toEnu(P.c);
                const double iA = 1.0 / (P.sA * P.sA), iB = 1.0 / (P.sB * P.sB), iC = 1.0 / (P.sZ * P.sZ);
                auto Mx = [&](int p, int q) { return iA * ae[p] * ae[q] + iB * be[p] * be[q] + iC * ce[p] * ce[q]; };
                GpuDustCloud &d = tail[1 + k];
                d.c = glm::vec4(glm::vec3(toEnu(rel)), (float)P.Sigma);
                d.ia = glm::vec4((float)Mx(0, 0), (float)Mx(1, 1), (float)Mx(2, 2),
                                 (float)(1.0 / (std::pow(2.0 * kPiL, 1.5) * P.sA * P.sB * P.sZ)));
                d.ib = glm::vec4((float)Mx(0, 1), (float)Mx(0, 2), (float)Mx(1, 2), 0.0f);
                d.col = glm::vec4(0.0f);
                d.cone = glm::vec4(0.0f);
                ++k;
            }
        }
        tail[0].c = glm::vec4((float)k, 0.0f, 0.0f, 0.0f);
    }

    if (!smokeLayerMapped)
        return;
    uint32_t *hdr = static_cast<uint32_t *>(smokeLayerMapped);
    float *hf = static_cast<float *>(smokeLayerMapped);
    hdr[0] = kSmokeLayerRes;
    hdr[1] = kSmokeLayerRes;
    hdr[2] = 0u;
    hdr[3] = 0u;
    gMap.on = false;
    if (!on || smokeLayerPipe == VK_NULL_HANDLE)
    {
        if (ctx_) ctx_->mirrorDirty(smokeLayerMapped, 0, kSmokeLayerHeader);
        return;
    }
    // The palls on the plane: centre, axes, the extent the map must cover
    struct PP
    {
        glm::dvec2 c, a, b;
        double sA, sB, peak, zM, sZ;
    };
    std::vector<PP> pp;
    double half = 300e3;
    for (const SmokePall &P : smokePalls)
    {
        const double cu = glm::dot(P.c, up);
        if (cu < 0.25) continue;   // (past ~75 deg of arc: off the plane's useful part)
        PP q;
        q.c = glm::dvec2(glm::dot(P.c, E), glm::dot(P.c, N)) * (kReL / cu);
        glm::dvec2 a2(glm::dot(P.a, E), glm::dot(P.a, N));
        a2 = glm::length(a2) > 1e-9 ? glm::normalize(a2) : glm::dvec2(1.0, 0.0);
        q.a = a2;
        q.b = glm::dvec2(-a2.y, a2.x);
        // (the gnomonic plane stretches away from the eye by up to 1 / cos^2: the widths with it, the column kept)
        const double st = 1.0 / cu;
        q.sA = P.sA * st;
        q.sB = P.sB * st;
        q.peak = P.Sigma / (2.0 * kPiL * P.sA * P.sB);
        q.zM = P.zM;
        q.sZ = P.sZ;
        pp.push_back(q);
        half = std::max(half, glm::length(q.c) + 3.5 * std::max(q.sA, q.sB));
    }
    half = std::min(half, 4000e3);
    const int W = (int)kSmokeLayerRes;
    const double tx = 2.0 * half / W;
    static thread_local std::vector<float> sT, sZ1, sZ2;
    sT.assign((size_t)W * W, 0.0f);
    sZ1.assign((size_t)W * W, 0.0f);
    sZ2.assign((size_t)W * W, 0.0f);
    for (const PP &q : pp)
    {
        const double R = 3.5 * std::max(q.sA, q.sB);
        const int i0 = std::max(0, (int)std::floor((q.c.x - R + half) / tx));
        const int i1 = std::min(W - 1, (int)std::ceil((q.c.x + R + half) / tx));
        const int j0 = std::max(0, (int)std::floor((q.c.y - R + half) / tx));
        const int j1 = std::min(W - 1, (int)std::ceil((q.c.y + R + half) / tx));
        const double ia = 1.0 / (q.sA * q.sA), ib = 1.0 / (q.sB * q.sB);
        const double z2 = q.sZ * q.sZ + q.zM * q.zM;
        for (int j = j0; j <= j1; ++j)
            for (int i = i0; i <= i1; ++i)
            {
                const glm::dvec2 d = glm::dvec2((i + 0.5) * tx - half, (j + 0.5) * tx - half) - q.c;
                const double u = glm::dot(d, q.a), v = glm::dot(d, q.b);
                const double e = 0.5 * (u * u * ia + v * v * ib);
                if (e > 6.2) continue;
                // (less its value at the cut, 3.5 sigma: no step there, which a grazing ray from orbit multiplies ~30x)
                const double w = q.peak * (std::exp(-e) - 0.0020294);
                const size_t k = (size_t)j * W + i;
                sT[k] += (float)w;
                sZ1[k] += (float)(w * q.zM);
                sZ2[k] += (float)(w * z2);
            }
    }
    uint32_t *tex = reinterpret_cast<uint32_t *>(static_cast<char *>(smokeLayerMapped) + kSmokeLayerHeader);
    double top = 0.0, maxTau = 0.0;
    gMap.E = E, gMap.N = N, gMap.U = up, gMap.half = half, gMap.W = W;
    gMap.tau.assign((size_t)W * W, 0.0f);
    gMap.zm.assign((size_t)W * W, 2000.0f);
    gMap.sz.assign((size_t)W * W, 800.0f);
    for (size_t k = 0; k < (size_t)W * W; ++k)
    {
        const double t = sT[k];
        if (!(t > 1e-5))
        {
            tex[k] = 0u;
            continue;
        }
        const double zm = sZ1[k] / t;
        const double sz = std::sqrt(std::max(sZ2[k] / t - zm * zm, 150.0 * 150.0));
        const uint32_t h = glm::packHalf1x16((float)std::min(t, 6.0e4));
        const uint32_t zq = (uint32_t)std::clamp(std::lround(zm / 40.0), 0l, 255l);
        const uint32_t sq = (uint32_t)std::clamp(std::lround(sz / 20.0), 5l, 255l);
        tex[k] = h | (zq << 16u) | (sq << 24u);
        gMap.tau[k] = (float)t, gMap.zm[k] = (float)(zq * 40.0), gMap.sz[k] = (float)std::max(sq * 20.0, 100.0);
        maxTau = std::max(maxTau, t);
        if (t > 1e-3) top = std::max(top, zq * 40.0 + 3.0 * sq * 20.0);
    }
    smokeLayerAny = maxTau > 1e-3;
    gMap.on = smokeLayerAny;
    smokeLayerMaxTau = (float)maxTau;
    smokeLayerTopM = (float)std::min(top, 12000.0);
    smokeLayerHalfM = (float)half;
    hdr[2] = smokeLayerAny ? 1u : 0u;
    hf[4] = (float)half;
    hf[5] = smokeLayerTopM;
    hf[6] = (float)maxTau;
    hf[7] = 0.0f;
    hf[8] = kLayerAlbedo;
    hf[9] = std::max(wildfireSmokeSpectral, 0.0f) * kLayerSpecK;
    hf[10] = kLayerG;
    hf[11] = kLayerSkyShare;
    hf[12] = std::max(wildfireSmokeBright, 0.0f);
    hf[13] = hf[14] = hf[15] = 0.0f;
    if (ctx_) ctx_->mirrorDirty(smokeLayerMapped, 0, kSmokeLayerHeader + (VkDeviceSize)W * W * sizeof(uint32_t));
    smokeLayerBuildMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

void SatelliteSim::recordSmokeLayer(VkCommandBuffer cmd, const CloudMarchPC &cpc)
{
    if (!smokeLayerPipe || !smokeLayerAny || wildfireSmokeLayer <= 0.0f || potatoSkyOn())
        return;
    FPROF_SCOPE("smokeLayer.record");
    VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &mb, 0,
                         nullptr, 0, nullptr);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, smokeLayerPipe);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, smokeLayerPipeLayout, 0, 1, &smokeLayerDescSet, 0, nullptr);
    vkCmdPushConstants(cmd, smokeLayerPipeLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(cpc), &cpc);
    vkCmdDispatch(cmd, (computeCloudExtentBuilt.width + 7) / 8, (computeCloudExtentBuilt.height + 7) / 8, 1);
}

void SatelliteSim::destroySmokeLayer(VkDevice dev)
{
    if (smokeLayerPipe)
        vkDestroyPipeline(dev, smokeLayerPipe, nullptr);
    if (smokeLayerPipeLayout)
        vkDestroyPipelineLayout(dev, smokeLayerPipeLayout, nullptr);
    if (smokeLayerDescPool)
        vkDestroyDescriptorPool(dev, smokeLayerDescPool, nullptr);
    if (smokeLayerDescLayout)
        vkDestroyDescriptorSetLayout(dev, smokeLayerDescLayout, nullptr);
    smokeLayerPipe = VK_NULL_HANDLE;
    smokeLayerPipeLayout = VK_NULL_HANDLE;
    smokeLayerDescPool = VK_NULL_HANDLE;
    smokeLayerDescLayout = VK_NULL_HANDLE;
    smokeLayerDescSet = VK_NULL_HANDLE;
    if (smokeLayerBuf != VK_NULL_HANDLE)   // (as the noctilucent map's: the mirror is torn down with the context)
    {
        vkDestroyBuffer(dev, smokeLayerBuf, nullptr);
        vkFreeMemory(dev, smokeLayerMem, nullptr);
        smokeLayerBuf = VK_NULL_HANDLE;
        smokeLayerMem = VK_NULL_HANDLE;
        smokeLayerMapped = nullptr;
    }
}
