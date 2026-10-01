// SatelliteSim — exposure (review 11, the user: "real brightness and metering is the way to go").
//
// One exposure for the frame, from one of four modes (SatelliteSim.h, "Exposure"): Manual (an EV100),
// Auto (a histogram meter on the HDR frame), HDR full and HDR eye (auto + local adaptation, the eye's
// limits for the second), plus a Legacy mode that reproduces the old day/night ramp for A/B.
//
// The HDR frame is the sky TAA path's: sat_sky.frag -DSKY_TAA writes pre-exposed radiance, sky_taa.comp
// resolves it (rescaling its history by the exposure's change), and sky_tonemap.comp tone-maps it into
// skyLdrImg — blitted into the swapchain — while building the meter's histogram of log2(scene luminance).
// Where that path does not run (render scale < 1, the Lite/Potato skies) the meter falls back to the
// displayed frame read back at 64x36 and un-tone-mapped (readExposureMeter).
#include "SatelliteSim.h"
#include "Log.h"
#include <glm/gtc/packing.hpp>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>

// The old exposure: EXPOSURE_NIGHT 10 .. EXPOSURE_DAY 1.8 on the Sun's elevation at the observer, x the
// exposure shift. The point sources' magnitudes and the Legacy mode are measured against it.
float SatelliteSim::exposureLegacyE() const
{
    const float dayness = glm::clamp((sunDirENU.w + 0.2f) / 1.2f, 0.0f, 1.0f);
    return glm::mix(10.0f, 1.8f, powf(dayness, 0.4f)) * exp2f(cv2ExposureEV);
}

// The scene's metered EV100 from the histogram: the average log luminance over the frame's upper range.
// Pixels more than 16 EV below its brightest 2% are ignored (black space around a small Earth), then the
// darkest and brightest 10% of what is left (a standard camera meter's band; 40-95% let a bright dusk horizon
// set the exposure and the sky above it went black); true black (bin 0) never counts. EV100 = log2(L / 0.125 cd/m2), the reflected-light meter's calibration (K = 12.5).
bool SatelliteSim::meterEV100FromHistogram(float &ev) const
{
    double total = 0.0;
    for (int b = 1; b < kExpHistBins; ++b)
        total += expHist[b];
    if (total <= 0.0)
    {
        // Nothing above black. Truly black (no light at all) stays black at any exposure, but the display
        // readback fallback quantises a night drawn at a day exposure to 0 everywhere — meter it as very dark,
        // so the exposure opens up (to the mode's limit) until there is something to read.
        if (expHist[0] == 0)
            return false;
        ev = -20.0f;
        return true;
    }
    // The top 2%.
    int hi = kExpHistBins - 1;
    for (double acc = 0.0; hi > 1; --hi)
    {
        acc += expHist[hi];
        if (acc >= 0.02 * total)
            break;
    }
    const int lo = std::max(1, hi - (int)(16.0f * kExpHistBinsPerEV));
    double win = 0.0;
    for (int b = lo; b < kExpHistBins; ++b)
        win += expHist[b];
    if (win <= 0.0)
        return false;
    const double a0 = 0.10 * win, a1 = 0.90 * win;
    double acc = 0.0, sumW = 0.0, sumL = 0.0;
    for (int b = lo; b < kExpHistBins; ++b)
    {
        const double c = expHist[b];
        const double s0 = std::max(acc, a0), s1 = std::min(acc + c, a1);
        if (s1 > s0)
        {
            const double l2 = kExpHistMinLog2 + (b + 0.5) / kExpHistBinsPerEV;
            sumW += s1 - s0;
            sumL += (s1 - s0) * l2;
        }
        acc += c;
    }
    if (sumW <= 0.0)
        return false;
    const double log2L = sumL / sumW;                                  // sim units
    ev = (float)(log2L + std::log2((double)kCdPerSimUnit) + 3.0);     // log2(L_cd x 8)
    return true;
}

// Start of the frame: the mode's EV100 and the exposure multiplier the passes read (UBO exposureScale).
void SatelliteSim::updateExposure(float dt)
{
    exposurePrevE = exposureE;
    if (expHistFresh)
    {
        expHistFresh = false;
        float m;
        if (meterEV100FromHistogram(m))
        {
            exposureMeterEV100 = m;
            exposureMeterValid = true;
        }
    }
    const int mode = std::clamp(exposureMode, 0, (int)ExpLegacy);
    if (mode == ExpLegacy)
    {
        exposureE = exposureLegacyE();
        exposureEV100 = exposureEV100FromE(exposureE);
        return;
    }
    if (mode == ExpManual)
    {
        exposureEV100 = exposureManualEV100;
        exposureE = exposureEFromEV100(exposureEV100);
        return;
    }
    // Metered. The key: a dark scene is shown darker than mid-grey (night reads as night), by mode.
    const float m = exposureMeterEV100;
    float comp, evMin, evMax, tauDarken, tauBrighten;
    switch (mode)
    {
    case ExpHdrFull: comp = std::clamp((9.0f - m) * 0.2f, 0.0f, 2.0f); evMin = -10.0f; evMax = 21.0f; tauDarken = 0.6f; tauBrighten = 1.2f; break;
    case ExpHdrEye:  comp = std::clamp((9.0f - m) * 0.4f, 0.0f, 4.5f); evMin = -3.5f;  evMax = 21.0f; tauDarken = 0.5f; tauBrighten = 4.0f; break;
    default:         comp = std::clamp((9.0f - m) * 0.3f, 0.0f, 3.0f); evMin = -6.0f;  evMax = 21.0f; tauDarken = 0.6f; tauBrighten = 1.2f; break;
    }
    const float target = std::clamp(m + comp - cv2ExposureEV, evMin, evMax);
    if (!exposureMeterValid)
        ; // nothing metered yet: hold
    else if (!std::isfinite(exposureEV100) || std::fabs(target - exposureEV100) > 30.0f)
        exposureEV100 = target;
    else
    {
        const float tau = target > exposureEV100 ? tauDarken : tauBrighten;
        exposureEV100 += (target - exposureEV100) * (1.0f - expf(-std::max(dt, 0.0f) / tau));
    }
    exposureE = exposureEFromEV100(exposureEV100);
}

// The status bar's mode button: Manual -> Auto -> HDR full -> HDR eye -> Manual (Legacy is harness-only and
// steps to Manual). Entering Manual keeps the current exposure, so the picture does not jump.
void SatelliteSim::cycleExposureMode()
{
    const int next = (exposureMode >= ExpHdrEye) ? ExpManual : exposureMode + 1;
    if (next == ExpManual)
        exposureManualEV100 = std::round(exposureEV100 * 3.0f) / 3.0f;
    exposureMode = next;
}

// The status bar's -/+: a third of a stop darker/brighter. Manual moves its EV100 (a higher EV is darker);
// the metered modes their exposure shift.
void SatelliteSim::stepExposure(int dir)
{
    const float third = 1.0f / 3.0f;
    if (exposureMode == ExpManual)
        exposureManualEV100 = std::clamp(std::round((exposureManualEV100 - dir * third) * 3.0f) / 3.0f, -12.0f, 24.0f);
    else
        cv2ExposureEV = std::clamp(std::round((cv2ExposureEV + dir * third) * 3.0f) / 3.0f, -6.0f, 6.0f);
}

// After the fence: the HDR path's histogram, and a pending harness radiance probe.
void SatelliteSim::readExposureHistogram()
{
    if (expHistPending && expHistMapped)
    {
        memcpy(expHist, expHistMapped, sizeof(expHist));
        expHistFresh = true;
        meterFromHdr = true;
        expHistPending = false;
    }
    if (radiancePending && radianceReadMem && ctx_)
    {
        radiancePending = false;
        void *mp = nullptr;
        if (vkMapMemory(ctx_->device, radianceReadMem, 0, VK_WHOLE_SIZE, 0, &mp) == VK_SUCCESS && mp)
        {
            const uint16_t *h = (const uint16_t *)mp;
            const int n = radiancePendingSize * radiancePendingSize;
            double s[3] = {0, 0, 0};
            float mx = 0.0f;
            for (int i = 0; i < n; ++i)
            {
                float c[3];
                for (int k = 0; k < 3; ++k)
                    c[k] = glm::unpackHalf1x16(h[i * 4 + k]);
                for (int k = 0; k < 3; ++k)
                    s[k] += c[k];
                mx = std::max(mx, 0.2126f * c[0] + 0.7152f * c[1] + 0.0722f * c[2]);
            }
            vkUnmapMemory(ctx_->device, radianceReadMem);
            // Pre-exposed -> scene radiance: divide by the exposure that frame was rendered with.
            const float e = std::max(radianceReqE, 1e-30f);
            for (int k = 0; k < 3; ++k)
                radianceResult[k] = (float)(s[k] / std::max(n, 1)) / e;
            radianceResult[3] = 0.2126f * radianceResult[0] + 0.7152f * radianceResult[1] + 0.0722f * radianceResult[2];
            radianceResultMax = mx / e;
            radianceResultValid = true;
        }
    }
}

static void expoMemBarrier(VkCommandBuffer cmd, VkAccessFlags srcA, VkAccessFlags dstA,
                           VkPipelineStageFlags srcS, VkPipelineStageFlags dstS)
{
    VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    mb.srcAccessMask = srcA;
    mb.dstAccessMask = dstA;
    vkCmdPipelineBarrier(cmd, srcS, dstS, 0, 1, &mb, 0, nullptr, 0, nullptr);
}

void SatelliteSim::createSkyToneResources(VulkanContext &ctx)
{
    const uint32_t W = ctx.swapExtent.width, H = ctx.swapExtent.height;
    auto makeView = [&](VkImage img, VkFormat fmt) {
        VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vci.image = img;
        vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vci.format = fmt;
        vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkImageView v = VK_NULL_HANDLE;
        vkCreateImageView(ctx.device, &vci, nullptr, &v);
        return v;
    };
    ctx.createImage(W, H, VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                    skyLdrImg, skyLdrMem);
    skyLdrView = makeView(skyLdrImg, VK_FORMAT_R16G16B16A16_SFLOAT);
    lumCellW = (W + 15) / 16;
    lumCellH = (H + 15) / 16;
    ctx.createImage(lumCellW, lumCellH, VK_FORMAT_R32_SFLOAT, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                    lumCellImg, lumCellMem);
    ctx.createImage(lumCellW, lumCellH, VK_FORMAT_R32_SFLOAT, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                    lumBlurImg, lumBlurMem);
    lumCellView = makeView(lumCellImg, VK_FORMAT_R32_SFLOAT);
    lumBlurView = makeView(lumBlurImg, VK_FORMAT_R32_SFLOAT);
    {
        VkCommandBuffer c = ctx.beginOneTimeCommands();
        for (VkImage img : {skyLdrImg, lumCellImg, lumBlurImg})
            ctx.imageBarrier(c, img, 0, VK_ACCESS_SHADER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                             VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
        ctx.endOneTimeCommands(c);
    }
    {
        VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        si.magFilter = si.minFilter = VK_FILTER_NEAREST;
        si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        vkCreateSampler(ctx.device, &si, nullptr, &lumLinearSampler);
    }
    if (!expHistBuf)
    {
        ctx.createBuffer(sizeof(uint32_t) * kExpHistBins,
                         VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                         VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, expHistBuf, expHistMem);
        ctx.createBuffer(sizeof(uint32_t) * kExpHistBins, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, expHistReadBuf, expHistReadMem);
        vkMapMemory(ctx.device, expHistReadMem, 0, VK_WHOLE_SIZE, 0, &expHistMapped);
        ctx.createBuffer(64 * 64 * 8, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, radianceReadBuf, radianceReadMem);
    }

    // Descriptor layouts: the tone pass (hdr, ldr, lumBlur) and the luminance pass (hdr, cell, blur, histogram).
    auto layoutOf = [&](const VkDescriptorType *types, uint32_t n, VkDescriptorSetLayout &out) {
        VkDescriptorSetLayoutBinding b[4] = {};
        for (uint32_t i = 0; i < n; ++i)
            b[i] = {i, types[i], 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
        VkDescriptorSetLayoutCreateInfo lci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        lci.bindingCount = n;
        lci.pBindings = b;
        vkCreateDescriptorSetLayout(ctx.device, &lci, nullptr, &out);
    };
    const VkDescriptorType toneT[3] = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                                       VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER};
    const VkDescriptorType lumT[4] = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                                      VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER};
    layoutOf(toneT, 3, skyToneLayout);
    layoutOf(lumT, 4, skyLumLayout);
    VkDescriptorPoolSize sizes[3] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 6},
                                     {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 6},
                                     {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2}};
    VkDescriptorPoolCreateInfo pci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pci.maxSets = 4;
    pci.poolSizeCount = 3;
    pci.pPoolSizes = sizes;
    vkCreateDescriptorPool(ctx.device, &pci, nullptr, &skyTonePool);
    VkDescriptorSetLayout layouts[4] = {skyToneLayout, skyToneLayout, skyLumLayout, skyLumLayout};
    VkDescriptorSet sets[4];
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = skyTonePool;
    ai.descriptorSetCount = 4;
    ai.pSetLayouts = layouts;
    vkAllocateDescriptorSets(ctx.device, &ai, sets);
    for (int k = 0; k < 2; ++k)
    {
        skyToneSet[k] = sets[k];
        skyLumSet[k] = sets[2 + k];
        // The resolve bound with set k writes history image 1 - k: that is what these sets read.
        VkDescriptorImageInfo src{skyTaaNearestSampler, skyTaaHistColorView[1 - k], VK_IMAGE_LAYOUT_GENERAL};
        VkDescriptorImageInfo ldr{VK_NULL_HANDLE, skyLdrView, VK_IMAGE_LAYOUT_GENERAL};
        VkDescriptorImageInfo blurS{lumLinearSampler, lumBlurView, VK_IMAGE_LAYOUT_GENERAL};
        VkDescriptorImageInfo cellI{VK_NULL_HANDLE, lumCellView, VK_IMAGE_LAYOUT_GENERAL};
        VkDescriptorImageInfo blurI{VK_NULL_HANDLE, lumBlurView, VK_IMAGE_LAYOUT_GENERAL};
        VkDescriptorBufferInfo hb{expHistBuf, 0, VK_WHOLE_SIZE};
        VkWriteDescriptorSet w[7] = {};
        auto wr = [&](int i, VkDescriptorSet set, uint32_t binding, VkDescriptorType t) {
            w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w[i].dstSet = set;
            w[i].dstBinding = binding;
            w[i].descriptorCount = 1;
            w[i].descriptorType = t;
        };
        wr(0, skyToneSet[k], 0, toneT[0]); w[0].pImageInfo = &src;
        wr(1, skyToneSet[k], 1, toneT[1]); w[1].pImageInfo = &ldr;
        wr(2, skyToneSet[k], 2, toneT[2]); w[2].pImageInfo = &blurS;
        wr(3, skyLumSet[k], 0, lumT[0]); w[3].pImageInfo = &src;
        wr(4, skyLumSet[k], 1, lumT[1]); w[4].pImageInfo = &cellI;
        wr(5, skyLumSet[k], 2, lumT[2]); w[5].pImageInfo = &blurI;
        wr(6, skyLumSet[k], 3, lumT[3]); w[6].pBufferInfo = &hb;
        vkUpdateDescriptorSets(ctx.device, 7, w, 0, nullptr);
    }
    auto pipeOf = [&](VkDescriptorSetLayout l, const char *spv, VkPipelineLayout &pl, VkPipeline &p) {
        VkPushConstantRange pr{VK_SHADER_STAGE_COMPUTE_BIT, 0, 32};
        VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        plci.setLayoutCount = 1;
        plci.pSetLayouts = &l;
        plci.pushConstantRangeCount = 1;
        plci.pPushConstantRanges = &pr;
        vkCreatePipelineLayout(ctx.device, &plci, nullptr, &pl);
        VkShaderModule cm = ctx.loadShader(spv);
        VkComputePipelineCreateInfo cci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        cci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, cm, "main", nullptr};
        cci.layout = pl;
        if (vkCreateComputePipelines(ctx.device, VK_NULL_HANDLE, 1, &cci, nullptr, &p) != VK_SUCCESS)
            throw std::runtime_error(std::string("SatelliteSim: failed to create ") + spv);
        vkDestroyShaderModule(ctx.device, cm, nullptr);
    };
    pipeOf(skyToneLayout, "shaders/sky_tonemap.comp.spv", skyTonePipeLayout, skyTonePipeline);
    pipeOf(skyLumLayout, "shaders/sky_lum.comp.spv", skyLumPipeLayout, skyLumPipeline);
}

void SatelliteSim::destroySkyToneResources(VkDevice device)
{
    auto dp = [&](VkPipeline &p) { if (p) vkDestroyPipeline(device, p, nullptr); p = VK_NULL_HANDLE; };
    auto dl = [&](VkPipelineLayout &p) { if (p) vkDestroyPipelineLayout(device, p, nullptr); p = VK_NULL_HANDLE; };
    auto dsl = [&](VkDescriptorSetLayout &p) { if (p) vkDestroyDescriptorSetLayout(device, p, nullptr); p = VK_NULL_HANDLE; };
    auto di = [&](VkImage &img, VkDeviceMemory &mem, VkImageView &v) {
        if (v) vkDestroyImageView(device, v, nullptr);
        if (img) vkDestroyImage(device, img, nullptr);
        if (mem) vkFreeMemory(device, mem, nullptr);
        v = VK_NULL_HANDLE; img = VK_NULL_HANDLE; mem = VK_NULL_HANDLE;
    };
    dp(skyTonePipeline);
    dp(skyLumPipeline);
    dl(skyTonePipeLayout);
    dl(skyLumPipeLayout);
    if (skyTonePool) vkDestroyDescriptorPool(device, skyTonePool, nullptr);
    skyTonePool = VK_NULL_HANDLE;
    dsl(skyToneLayout);
    dsl(skyLumLayout);
    if (lumLinearSampler) vkDestroySampler(device, lumLinearSampler, nullptr);
    lumLinearSampler = VK_NULL_HANDLE;
    di(skyLdrImg, skyLdrMem, skyLdrView);
    di(lumCellImg, lumCellMem, lumCellView);
    di(lumBlurImg, lumBlurMem, lumBlurView);
    // The histogram and readback buffers are swapchain-independent: freed with the sim (cleanup), not on resize.
    expHistPending = false;
    radiancePending = false;
}

// After the resolve (set k wrote history 1 - k): meter it and build the local adaptation map (sky_lum.comp),
// tone-map it into skyLdrImg (sky_tonemap.comp), serve a pending radiance probe. The caller blits skyLdrImg.
void SatelliteSim::recordSkyTone(VkCommandBuffer cmd, VulkanContext &ctx, int k)
{
    VkImage hist = skyTaaHistColorImg[1 - k];
    ctx.imageBarrier(cmd, hist, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT,
                     VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
                     VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT);
    vkCmdFillBuffer(cmd, expHistBuf, 0, VK_WHOLE_SIZE, 0);
    VkBufferMemoryBarrier bb{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
    bb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    bb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    bb.srcQueueFamilyIndex = bb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    bb.buffer = expHistBuf;
    bb.size = VK_WHOLE_SIZE;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 1, &bb, 0, nullptr);

    struct { glm::vec4 expo, mode; } pc{};
    // The meter + the cells, then their blur.
    pc.expo = glm::vec4(exposureE, 0.0f, kExpHistMinLog2, kExpHistBinsPerEV);
    pc.mode = glm::vec4(0.0f);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, skyLumPipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, skyLumPipeLayout, 0, 1, &skyLumSet[k], 0, nullptr);
    vkCmdPushConstants(cmd, skyLumPipeLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
    vkCmdDispatch(cmd, lumCellW, lumCellH, 1);
    expoMemBarrier(cmd, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT,
                   VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT);
    pc.mode.x = 1.0f;
    vkCmdPushConstants(cmd, skyLumPipeLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
    vkCmdDispatch(cmd, (lumCellW + 15) / 16, (lumCellH + 15) / 16, 1);
    VkBufferCopy bc{0, 0, sizeof(uint32_t) * kExpHistBins};
    vkCmdCopyBuffer(cmd, expHistBuf, expHistReadBuf, 1, &bc);
    expHistPending = true;
    expoMemBarrier(cmd, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                   VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);

    // The tone pass.
    const int mode = std::clamp(exposureMode, 0, (int)ExpLegacy);
    float strength = 0.0f, maxStops = 0.0f;
    if (mode == ExpHdrFull) { strength = hdrLocalStrength; maxStops = hdrLocalMaxStops; }
    if (mode == ExpHdrEye)  { strength = eyeLocalStrength; maxStops = eyeLocalMaxStops; }
    pc.expo = glm::vec4(exposureE, std::clamp(cv2HighlightRolloff, 0.0f, 1.0f), std::clamp(strength, 0.0f, 1.0f), maxStops);
    pc.mode = glm::vec4((float)mode, kCdPerSimUnit, std::clamp(eyeNightVision, 0.0f, 1.0f), 0.0f);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, skyTonePipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, skyTonePipeLayout, 0, 1, &skyToneSet[k], 0, nullptr);
    vkCmdPushConstants(cmd, skyTonePipeLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
    vkCmdDispatch(cmd, (ctx.swapExtent.width + 15) / 16, (ctx.swapExtent.height + 15) / 16, 1);

    // Harness `radiance`: a square of the resolved HDR frame, read after the fence.
    if (radianceReq[0] >= 0 && radianceReadBuf)
    {
        const int s = std::clamp(radianceReq[2], 1, 64);
        const int x = std::clamp(radianceReq[0] - s / 2, 0, (int)ctx.swapExtent.width - s);
        const int y = std::clamp(radianceReq[1] - s / 2, 0, (int)ctx.swapExtent.height - s);
        VkBufferImageCopy r{};
        r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        r.imageOffset = {x, y, 0};
        r.imageExtent = {(uint32_t)s, (uint32_t)s, 1};
        vkCmdCopyImageToBuffer(cmd, hist, VK_IMAGE_LAYOUT_GENERAL, radianceReadBuf, 1, &r);
        radiancePending = true;
        radiancePendingSize = s;
        radianceReqE = exposureE;
        radianceReq[0] = -1;
    }
    ctx.imageBarrier(cmd, skyLdrImg, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                     VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
                     VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
}
