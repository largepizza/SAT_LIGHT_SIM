// Noctilucent clouds (2026-11-16, .plans/PLUME_OVERHAUL_PLAN.md phase 3; issue_20261115_165612 "Noctilucent cloud
// request"; the user's decision: physical, "the launch rate is the physics"). The physics is Nlc.h; this file keeps the
// launches' water deposits (launchWaterDeposits, cached by batch), builds the global map on a worker (vapour: the
// background + every deposit drifted, spread and photolysed to now; the mean temperature with the tides; the frost
// point), uploads it (a mirrored storage buffer: sat_sky.frag is at its sampled-image floor) and records
// shaders/nlc_shell.comp on the cloud composite's targets. docs/subsystems/launches.md "Launch-made noctilucent clouds".

#include "SatelliteSim.h"
#include "../VulkanContext.h"
#include "../FrameProfile.h"
#include "../Log.h"
#include <glm/gtc/packing.hpp>
#include <nlohmann/json.hpp>
#include <atomic>
#include <chrono>
#include <cstring>
#include <thread>

struct nlc::Job
{
    std::thread th;
    std::atomic<bool> done{false};
    double t = 0.0;
    double ms = 0.0;          // the build's wall time
    std::vector<nlc::Deposit> deps;
    std::vector<glm::uvec2> tex;            // the GPU texels
    std::vector<float> ppmv, tMean, tFrost; // the CPU copy (state queries)
    bool anyIce = false;
    double waterKg = 0.0;                   // the deposits' water left now
    ~Job()
    {
        if (th.joinable())
            th.join();
    }
};

namespace
{
// The frost point by table (log ppmv 0.01 .. 1e6): the bisection is ~100x slower and the map has 131k texels.
struct FrostLut
{
    static constexpr int N = 1024;
    float t[N];
    FrostLut()
    {
        for (int i = 0; i < N; ++i)
            t[i] = (float)nlc::frostPointK(std::exp(std::log(0.01) + (std::log(1e6) - std::log(0.01)) * i / (N - 1)));
    }
    float operator()(double ppmv) const
    {
        const double u = (std::log(std::max(ppmv, 0.01)) - std::log(0.01)) / (std::log(1e6) - std::log(0.01)) * (N - 1);
        const int i = std::clamp((int)u, 0, N - 2);
        const double f = std::clamp(u - i, 0.0, 1.0);
        return (float)(t[i] + (t[i + 1] - t[i]) * f);
    }
};
const FrostLut &frostLut()
{
    static const FrostLut lut;
    return lut;
}

double texLat(uint32_t j) { return (0.5 - (j + 0.5) / nlc::kMapH) * nlc::kPi; }
double texLon(uint32_t i) { return ((i + 0.5) / nlc::kMapW - 0.5) * 2.0 * nlc::kPi; }

void buildField(nlc::Job &J)
{
    const auto c0 = std::chrono::steady_clock::now();
    const uint32_t W = nlc::kMapW, H = nlc::kMapH;
    J.ppmv.assign((size_t)W * H, (float)nlc::kBgPpmv);
    J.tMean.resize((size_t)W * H);
    J.tFrost.resize((size_t)W * H);
    J.tex.resize((size_t)W * H);
    std::vector<double> coslat(H), sinlat(H), coslon(W), sinlon(W);
    for (uint32_t j = 0; j < H; ++j)
        coslat[j] = std::cos(texLat(j)), sinlat[j] = std::sin(texLat(j));
    for (uint32_t i = 0; i < W; ++i)
        coslon[i] = std::cos(texLon(i)), sinlon[i] = std::sin(texLon(i));
    // the deposits: a Gaussian column on the sphere each (distance by the chord: within 0.3% out to 3.5 sigma)
    const double dLat = nlc::kPi / H, dLon = 2.0 * nlc::kPi / W;
    J.waterKg = 0.0;
    for (const nlc::Deposit &d : J.deps)
    {
        double lat, lon, sig, m;
        if (!nlc::depositAt(d, J.t, lat, lon, sig, m))
            continue;
        J.waterKg += m;
        const double peak = nlc::ppmvOfColumn(m / (2.0 * nlc::kPi * sig * sig));
        if (peak < 0.01)
            continue;
        const double ux = std::cos(lat) * std::cos(lon), uy = std::cos(lat) * std::sin(lon), uz = std::sin(lat);
        const double a = std::min(3.5 * sig / nlc::kR, nlc::kPi);      // angular reach
        const double inv2s2 = nlc::kR * nlc::kR / (2.0 * sig * sig);
        const int j0 = std::clamp((int)std::floor((0.5 * nlc::kPi - (lat + a)) / dLat), 0, (int)H - 1);
        const int j1 = std::clamp((int)std::ceil((0.5 * nlc::kPi - (lat - a)) / dLat), 0, (int)H - 1);
        for (int j = j0; j <= j1; ++j)
        {
            // the longitudes within reach on this row (all of them near a pole)
            const double cl = coslat[j];
            int i0 = 0, i1 = (int)W - 1;
            if (a < 0.5 * nlc::kPi && cl > std::sin(a))
            {
                const double hw = std::asin(std::sin(a) / cl);
                const double ic = (lon / (2.0 * nlc::kPi) + 0.5) * W - 0.5;
                i0 = (int)std::floor(ic - hw / dLon) - 1;
                i1 = (int)std::ceil(ic + hw / dLon) + 1;
                if (i1 - i0 + 1 >= (int)W)
                    i0 = 0, i1 = (int)W - 1;
            }
            for (int ii = i0; ii <= i1; ++ii)
            {
                const uint32_t i = (uint32_t)(((ii % (int)W) + (int)W) % (int)W);
                const double px = cl * coslon[i], py = cl * sinlon[i], pz = sinlat[j];
                const double c2 = (px - ux) * (px - ux) + (py - uy) * (py - uy) + (pz - uz) * (pz - uz);
                const double e = c2 * inv2s2;
                if (e < 12.25 * 0.5 * 1.02)
                    J.ppmv[(size_t)j * W + i] += (float)(peak * std::exp(-e));
            }
        }
    }
    const FrostLut &fl = frostLut();
    J.anyIce = false;
    for (uint32_t j = 0; j < H; ++j)
        for (uint32_t i = 0; i < W; ++i)
        {
            const size_t k = (size_t)j * W + i;
            const float T = (float)nlc::tMeanK(texLat(j), texLon(i), J.t);
            const float Tf = fl(J.ppmv[k]);
            J.tMean[k] = T;
            J.tFrost[k] = Tf;
            J.anyIce = J.anyIce || Tf > T - 3.0f * (float)nlc::kGwRmsK;
            J.tex[k] = glm::uvec2(glm::packHalf2x16(glm::vec2(std::min(J.ppmv[k], 60000.0f), T)),
                                  glm::packHalf2x16(glm::vec2(Tf, 0.0f)));
        }
    J.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - c0).count();
    J.done.store(true, std::memory_order_release);
}

// The map at (lat, lon) rad: vapour, mean T, frost point (nearest texel)
void fieldAt(const nlc::Job &F, double lat, double lon, double &ppmv, double &tm, double &tf)
{
    const uint32_t W = nlc::kMapW, H = nlc::kMapH;
    const int j = std::clamp((int)((0.5 - lat / nlc::kPi) * H), 0, (int)H - 1);
    const int i = (((int)((lon / (2.0 * nlc::kPi) + 0.5) * W)) % (int)W + (int)W) % (int)W;
    const size_t k = (size_t)j * W + (size_t)i;
    ppmv = F.ppmv[k];
    tm = F.tMean[k];
    tf = F.tFrost[k];
}
} // namespace

void SatelliteSim::createNlc(VulkanContext &ctx)
{
    const VkDevice dev = ctx.device;
    const VkDeviceSize sz = kNlcHeader + (VkDeviceSize)nlc::kMapW * nlc::kMapH * sizeof(glm::uvec2);
    nlcMapped = createMirrored(ctx, sz, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, nlcBuf, nlcMem, false, "nlc");
    const VkDescriptorType kTypes[6] = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                                        VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER};
    VkDescriptorSetLayoutBinding b[6];
    for (uint32_t i = 0; i < 6; ++i)
        b[i] = {i, kTypes[i], 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    li.bindingCount = 6;
    li.pBindings = b;
    vkCreateDescriptorSetLayout(dev, &li, nullptr, &nlcDescLayout);
    VkDescriptorPoolSize ps[3] = {{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1},
                                  {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2},
                                  {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 3}};
    VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pi.maxSets = 1;
    pi.poolSizeCount = 3;
    pi.pPoolSizes = ps;
    vkCreateDescriptorPool(dev, &pi, nullptr, &nlcDescPool);
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = nlcDescPool;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &nlcDescLayout;
    vkAllocateDescriptorSets(dev, &ai, &nlcDescSet);
    VkPushConstantRange pcr{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(CloudMarchPC)};
    VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &nlcDescLayout;
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges = &pcr;
    vkCreatePipelineLayout(dev, &pli, nullptr, &nlcPipeLayout);
    const std::string err = buildNlcPipeline(ctx, "");
    if (!err.empty())
        Log::line("init: WARNING nlc_shell pipeline: " + err + " (noctilucent clouds off)");
    writeNlcDescriptors(ctx);
}

std::string SatelliteSim::buildNlcPipeline(VulkanContext &ctx, const std::string &spv)
{
    VkShaderModule mod = VK_NULL_HANDLE;
    try
    {
        mod = ctx.loadShader(spv.empty() ? std::string("shaders/nlc_shell.comp.spv") : spv);
    }
    catch (const std::exception &ex)
    {
        return ex.what();
    }
    VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    ci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, mod, "main", nullptr};
    ci.layout = nlcPipeLayout;
    VkPipeline p = VK_NULL_HANDLE;
    const VkResult r = vkCreateComputePipelines(ctx.device, ctx.pipelineCache, 1, &ci, nullptr, &p);
    vkDestroyShaderModule(ctx.device, mod, nullptr);
    if (r != VK_SUCCESS)
        return "pipeline creation failed";
    if (nlcPipe != VK_NULL_HANDLE)
        vkDestroyPipeline(ctx.device, nlcPipe, nullptr);
    nlcPipe = p;
    return "";
}

void SatelliteSim::writeNlcDescriptors(VulkanContext &ctx)
{
    if (!nlcDescSet || !cloudMarchTargetAView || !sceneDepthView || !plumeMieView || !cv2ShapeView)
        return;
    VkDescriptorBufferInfo bufI{nlcBuf, 0, VK_WHOLE_SIZE};
    VkDescriptorImageInfo tA{VK_NULL_HANDLE, cloudMarchTargetAView, VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorImageInfo tB{VK_NULL_HANDLE, cloudMarchTargetBView, VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorImageInfo depth{sceneDepthSampler, sceneDepthView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkDescriptorImageInfo mie{cv2ClampSampler, plumeMieView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};   // (a 1x1 stand-in when missing)
    // the clouds' baked noise volume (the cirrus fibres' source): the fine structure
    VkDescriptorImageInfo shape{cv2RepeatSampler, cv2ShapeView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    auto W = [&](uint32_t bind, VkDescriptorType t, const VkDescriptorImageInfo *ii, const VkDescriptorBufferInfo *bi) {
        return VkWriteDescriptorSet{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, nlcDescSet, bind, 0, 1, t, ii, bi, nullptr};
    };
    VkWriteDescriptorSet w[6] = {W(0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &bufI),
                                 W(1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &tA, nullptr),
                                 W(2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &tB, nullptr),
                                 W(3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &depth, nullptr),
                                 W(4, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &mie, nullptr),
                                 W(5, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &shape, nullptr)};
    frameSafeUpdateDescriptorSets(ctx.device, 6, w, 0, nullptr);
}

void SatelliteSim::nlcUpdate(VulkanContext &ctx)
{
    if (!nlcMapped)
        return;
    FPROF_SCOPE("nlc.update");
    const double t = (double)simDayJ2000 * 86400.0 + simSecInDay;
    uint32_t *hdr = reinterpret_cast<uint32_t *>(nlcMapped);
    // (not on the light tiers, launchPlumesDrawn: no deposits collected, no map built, nothing dispatched)
    const bool on = nlcOn && launchesEnabled && nlcPipe != VK_NULL_HANDLE && launchPlumesDrawn();
    // a finished build: its texels to the GPU
    if (nlcJob && nlcJob->done.load(std::memory_order_acquire))
    {
        if (nlcJob->th.joinable())
            nlcJob->th.join();
        std::memcpy(reinterpret_cast<char *>(nlcMapped) + kNlcHeader, nlcJob->tex.data(), nlcJob->tex.size() * sizeof(glm::uvec2));
        ctx.mirrorDirty(nlcMapped, kNlcHeader, nlcJob->tex.size() * sizeof(glm::uvec2));
        nlcAnyIce = nlcJob->anyIce;
        nlcField = std::move(nlcJob);
        nlcJob.reset();
    }
    // a new build each sim minute (and at once after a jump): the deposits here (the launch cache is main-thread),
    // the map on a worker
    if (on && !nlcJob && std::abs(t - nlcStartT) >= 60.0)
    {
        FPROF_SCOPE("nlc.deposits");
        launchWaterDeposits(t - nlc::kWindowS, t, nlcDeposits);
        auto job = std::make_shared<nlc::Job>();
        job->t = t;
        for (auto it = nlcDeposits.begin(); it != nlcDeposits.end();)
        {
            const nlc::Deposit &d = it->second;
            // forget launches long past (or far ahead: the enumeration reaches past t1 by the programs' raise days)
            if (d.t < t - nlc::kWindowS - 3.0 * 86400.0 || d.t > t + 60.0 * 86400.0)
            {
                it = nlcDeposits.erase(it);
                continue;
            }
            if (d.m > 0.0 && d.t <= t && d.t >= t - nlc::kWindowS)
                job->deps.push_back(d);
            ++it;
        }
        nlcStartT = t;
        nlc::Job *jp = job.get();
        job->th = std::thread([jp] { buildField(*jp); });
        nlcJob = std::move(job);
    }
    // the header, every frame (the waves' drift is deterministic in sim time: nlc_shell.comp nlcStructure)
    float h[20];
    std::memset(h, 0, sizeof(h));
    hdr[0] = nlc::kMapW;
    hdr[1] = nlc::kMapH;
    hdr[2] = on ? 1u : 0u;
    hdr[3] = (on && nlcField && nlcAnyIce) ? 1u : 0u;
    // the broad wave 7.2 K rms; with the bands' (2.5) and billows' (1.0) waves, 7.7 K (nlc::kGwRmsK 8)
    h[7] = 7.2f;
    h[14] = 1500.0f;                                   // the layer's half-width (m): ~2.5 km FWHM
    h[15] = plumeMieReff.empty() ? 0.0f : 1.0f;        // the ice's Mie table (plume_mie.glsl) loaded
    // the fine structure drifts with the wind: 40 m/s along the bands, 10 across (wrapped by 102400 km, a multiple of every
    // period it reads)
    auto wrapM = [](double v) { return (float)(v - 1.024e8 * std::floor(v / 1.024e8)); };
    h[16] = wrapM(40.0 * t), h[17] = wrapM(10.0 * t), h[18] = nlcFibres, h[19] = 0.0f;
    std::memcpy(reinterpret_cast<char *>(nlcMapped) + 16, h + 4, 64);
    ctx.mirrorDirty(nlcMapped, 0, kNlcHeader);
}

void SatelliteSim::recordNlc(VkCommandBuffer cmd, const CloudMarchPC &cpc)
{
    if (!nlcPipe || !nlcOn || !launchesEnabled || !nlcField || !nlcAnyIce || !launchPlumesDrawn())
        return;
    FPROF_SCOPE("nlc.record");
    VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &mb, 0,
                         nullptr, 0, nullptr);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, nlcPipe);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, nlcPipeLayout, 0, 1, &nlcDescSet, 0, nullptr);
    vkCmdPushConstants(cmd, nlcPipeLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(cpc), &cpc);
    vkCmdDispatch(cmd, (computeCloudExtentBuilt.width + 7) / 8, (computeCloudExtentBuilt.height + 7) / 8, 1);
}

void SatelliteSim::destroyNlc(VkDevice dev)
{
    nlcJob.reset();   // (joins)
    nlcField.reset();
    if (nlcPipe)
        vkDestroyPipeline(dev, nlcPipe, nullptr);
    if (nlcPipeLayout)
        vkDestroyPipelineLayout(dev, nlcPipeLayout, nullptr);
    if (nlcDescPool)
        vkDestroyDescriptorPool(dev, nlcDescPool, nullptr);
    if (nlcDescLayout)
        vkDestroyDescriptorSetLayout(dev, nlcDescLayout, nullptr);
    nlcPipe = VK_NULL_HANDLE;
    nlcPipeLayout = VK_NULL_HANDLE;
    nlcDescPool = VK_NULL_HANDLE;
    nlcDescLayout = VK_NULL_HANDLE;
    nlcDescSet = VK_NULL_HANDLE;
    if (nlcBuf != VK_NULL_HANDLE)   // (as the brick list's: the mirror is torn down with the context)
    {
        vkDestroyBuffer(dev, nlcBuf, nullptr);
        vkFreeMemory(dev, nlcMem, nullptr);
        nlcBuf = VK_NULL_HANDLE;
        nlcMem = VK_NULL_HANDLE;
        nlcMapped = nullptr;
    }
}

// harness `state` -> nlc: the model's answer where it matters. ice_chance = P(T < frost point) now (the gravity waves'
// spread, 8 K rms, round the mean with the tide now); ice_chance_day = its mean over the day's tide (the season now);
// natural_* the same with the background vapour alone (what the launches add).
nlohmann::json SatelliteSim::nlcStateJson() const
{
    nlohmann::json j;
    j["on"] = nlcOn;
    j["deposits_cached"] = nlcDeposits.size();
    if (!nlcField)
    {
        j["field"] = nullptr;
        return j;
    }
    const nlc::Job &F = *nlcField;
    j["field_t_j2000"] = F.t;
    j["build_ms"] = F.ms;
    j["deposits"] = F.deps.size();
    j["water_kt"] = F.waterKg / 1e6;
    j["any_ice_possible"] = F.anyIce;
    j["season"] = nlc::season(F.t);
    const double tfBg = nlc::frostPointK(nlc::kBgPpmv);
    j["frost_point_background_k"] = tfBg;
    auto at = [&](double latDeg, double lonDeg) {
        const double la = latDeg * nlc::kPi / 180.0, lo = lonDeg * nlc::kPi / 180.0;
        double ppmv, tm, tf;
        fieldAt(F, la, lo, ppmv, tm, tf);
        double day = 0.0, dayN = 0.0;
        const double tc = nlc::tClimK(la, nlc::season(F.t));
        for (int k = 0; k < 48; ++k)
        {
            const double tt = F.t + k * 1800.0;
            const double T = tc + nlc::tideK(la, lo, tt);
            day += nlc::iceChance(T, tf) / 48.0;
            dayN += nlc::iceChance(T, tfBg) / 48.0;
        }
        return nlohmann::json{{"lat_deg", latDeg}, {"lon_deg", lonDeg}, {"vapour_ppmv", ppmv}, {"t_mean_k", tm},
                              {"frost_point_k", tf}, {"ice_chance", nlc::iceChance(tm, tf)}, {"ice_chance_day", day},
                              {"natural_ice_chance_day", dayN}};
    };
    j["at_eye"] = at(obsLatDeg, obsLonDeg);
    j["sites"] = {{"starbase", at(25.997, -97.155)}, {"louisiana", at(29.575, -92.475)}, {"cape", at(28.6, -80.6)},
                  {"arctic_70n", at(70.0, -95.0)}, {"antarctic_70s", at(-70.0, -95.0)}};
    // the share of the Earth (by area) where ice is more likely than not now, and its expected share
    double aW = 0.0, aIce = 0.0, aExp = 0.0, aNat = 0.0, peakPpmv = 0.0, peakLat = 0.0, peakLon = 0.0;
    for (uint32_t jj = 0; jj < nlc::kMapH; ++jj)
    {
        const double w = std::cos(texLat(jj));
        for (uint32_t i = 0; i < nlc::kMapW; ++i)
        {
            const size_t k = (size_t)jj * nlc::kMapW + i;
            const double p = nlc::iceChance(F.tMean[k], F.tFrost[k]);
            aW += w;
            aIce += p > 0.5 ? w : 0.0;
            aExp += p * w;
            aNat += nlc::iceChance(F.tMean[k], tfBg) * w;
            if (F.ppmv[k] > peakPpmv)
                peakPpmv = F.ppmv[k], peakLat = texLat(jj), peakLon = texLon(i);
        }
    }
    j["ice_area_frac"] = aIce / aW;
    j["ice_area_expected_frac"] = aExp / aW;
    j["natural_ice_area_expected_frac"] = aNat / aW;
    j["vapour_peak"] = {{"ppmv", peakPpmv}, {"lat_deg", peakLat * 180.0 / nlc::kPi}, {"lon_deg", peakLon * 180.0 / nlc::kPi}};
    return j;
}
