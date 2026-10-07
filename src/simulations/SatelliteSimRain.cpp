// Rain, snow and diamond dust at the eye as PARTICLES (2026-10-04).
//
// One instanced draw in the main pass, after the sky TAA has resolved the frame (the drops are crisp, full
// resolution, and never smeared by the history): rain_particles.vert builds every drop from its instance index
// (nested world-fixed boxes about the eye, Marshall-Palmer sizes, terminal velocities, the rain map around the
// eye deciding which exist), draws it as the streak it sweeps over the shutter time with its real coverage, and
// lights it with the cloud march's rain sample at the eye (the lightning pass writes cv2RainKey / cv2RainAmb)
// through the rain's own phase function — the bows included (include/cv2_optics.glsl). The particles are the
// near part of the rain volume, drawn one drop at a time. It replaced cloud_march.comp's rainDrops.
#include "SatelliteSim.h"
#include "../VulkanContext.h"
#include "../Log.h"
#include <algorithm>
#include <cmath>
#include <stdexcept>

void SatelliteSim::createRainParticles(VulkanContext &ctx)
{
    const VkShaderStageFlags st = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutBinding b[4] = {
        {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, st, nullptr},          // CloudParams
        {1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, st, nullptr},          // CloudV2Params
        {2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, st, nullptr},          // the flash buffer (rain map, light at the eye)
        {3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, st, nullptr},  // the half-res scene depth
    };
    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    li.bindingCount = 4;
    li.pBindings = b;
    if (vkCreateDescriptorSetLayout(ctx.device, &li, nullptr, &rainDescLayout) != VK_SUCCESS)
        throw std::runtime_error("SatelliteSim: rain particle descriptor layout");

    VkDescriptorPoolSize ps[3] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 2},
                                  {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1},
                                  {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1}};
    VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pi.maxSets = 1;
    pi.poolSizeCount = 3;
    pi.pPoolSizes = ps;
    if (vkCreateDescriptorPool(ctx.device, &pi, nullptr, &rainDescPool) != VK_SUCCESS)
        throw std::runtime_error("SatelliteSim: rain particle descriptor pool");
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = rainDescPool;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &rainDescLayout;
    if (vkAllocateDescriptorSets(ctx.device, &ai, &rainDescSet) != VK_SUCCESS)
        throw std::runtime_error("SatelliteSim: rain particle descriptor set");

    VkPushConstantRange pcr{st, 0, sizeof(RainDrawPC)};
    VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pl.setLayoutCount = 1;
    pl.pSetLayouts = &rainDescLayout;
    pl.pushConstantRangeCount = 1;
    pl.pPushConstantRanges = &pcr;
    if (vkCreatePipelineLayout(ctx.device, &pl, nullptr, &rainPipeLayout) != VK_SUCCESS)
        throw std::runtime_error("SatelliteSim: rain particle pipeline layout");

    createRainPipeline(ctx);
    writeRainDescriptors(ctx);
}

// The viewport is baked in (as every graphics pipeline here): recreated on a resize.
void SatelliteSim::createRainPipeline(VulkanContext &ctx)
{
    VkShaderModule vert = ctx.loadShader("shaders/rain_particles.vert.spv");
    VkShaderModule frag = ctx.loadShader("shaders/rain_particles.frag.spv");
    VkPipelineShaderStageCreateInfo stages[2] = {};
    stages[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, vert, "main", nullptr};
    stages[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, frag, "main", nullptr};

    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
    VkViewport vp{0, 0, (float)ctx.swapExtent.width, (float)ctx.swapExtent.height, 0, 1};
    VkRect2D sc{{0, 0}, ctx.swapExtent};
    VkPipelineViewportStateCreateInfo vps{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vps.viewportCount = 1;
    vps.pViewports = &vp;
    vps.scissorCount = 1;
    vps.pScissors = &sc;
    VkPipelineRasterizationStateCreateInfo rast{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rast.polygonMode = VK_POLYGON_MODE_FILL;
    rast.cullMode = VK_CULL_MODE_NONE;
    rast.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rast.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    // Tested against the unified scene depth at the drop's range (terrain, opaque cloud, a mesh in front hide it).
    VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    ds.depthTestEnable = VK_TRUE;
    ds.depthWriteEnable = VK_FALSE;
    ds.depthCompareOp = VK_COMPARE_OP_LESS;
    // Premultiplied over: the drop covers alpha of the pixel with its own (tonemapped) light.
    VkPipelineColorBlendAttachmentState cba{};
    cba.blendEnable = VK_TRUE;
    cba.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
    cba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    cba.colorBlendOp = VK_BLEND_OP_ADD;
    cba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    cba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    cba.alphaBlendOp = VK_BLEND_OP_ADD;
    cba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT |
                         VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = 1;
    cb.pAttachments = &cba;

    VkGraphicsPipelineCreateInfo ci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    ci.stageCount = 2;
    ci.pStages = stages;
    ci.pVertexInputState = &vi;
    ci.pInputAssemblyState = &ia;
    ci.pViewportState = &vps;
    ci.pRasterizationState = &rast;
    ci.pMultisampleState = &ms;
    ci.pDepthStencilState = &ds;
    ci.pColorBlendState = &cb;
    ci.layout = rainPipeLayout;
    ci.renderPass = ctx.renderPass;
    ci.subpass = 0;
    if (vkCreateGraphicsPipelines(ctx.device, ctx.pipelineCache, 1, &ci, nullptr, &rainPipeline) != VK_SUCCESS)
        throw std::runtime_error("SatelliteSim: rain particle pipeline");
    vkDestroyShaderModule(ctx.device, vert, nullptr);
    vkDestroyShaderModule(ctx.device, frag, nullptr);
}

// After any recreation of the scene depth image (recreateComputeScaledTargets).
void SatelliteSim::writeRainDescriptors(VulkanContext &ctx)
{
    if (!rainDescSet || !cloudParamsBuf || !cv2ParamsBuf || !cv2FlashBuf || !sceneDepthView)
        return;
    VkDescriptorBufferInfo cp{cloudParamsBuf, 0, sizeof(GpuCloudParams)};
    VkDescriptorBufferInfo v2{cv2ParamsBuf, 0, sizeof(GpuCloudV2Params)};
    VkDescriptorBufferInfo fl{cv2FlashBuf, 0, VK_WHOLE_SIZE};
    VkDescriptorImageInfo dp{sceneDepthSampler, sceneDepthView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet w[4] = {};
    for (int i = 0; i < 4; ++i)
    {
        w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w[i].dstSet = rainDescSet;
        w[i].dstBinding = (uint32_t)i;
        w[i].descriptorCount = 1;
    }
    w[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    w[0].pBufferInfo = &cp;
    w[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    w[1].pBufferInfo = &v2;
    w[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    w[2].pBufferInfo = &fl;
    w[3].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    w[3].pImageInfo = &dp;
    vkUpdateDescriptorSets(ctx.device, 4, w, 0, nullptr);
}

void SatelliteSim::destroyRainParticles(VkDevice device)
{
    if (rainPipeline) vkDestroyPipeline(device, rainPipeline, nullptr);
    if (rainPipeLayout) vkDestroyPipelineLayout(device, rainPipeLayout, nullptr);
    if (rainDescPool) vkDestroyDescriptorPool(device, rainDescPool, nullptr);
    if (rainDescLayout) vkDestroyDescriptorSetLayout(device, rainDescLayout, nullptr);
    rainPipeline = VK_NULL_HANDLE;
    rainPipeLayout = VK_NULL_HANDLE;
    rainDescPool = VK_NULL_HANDLE;
    rainDescSet = VK_NULL_HANDLE;
    rainDescLayout = VK_NULL_HANDLE;
}

// Inside the main render pass, after everything else in the scene (recordDraw).
void SatelliteSim::recordRainParticles(VkCommandBuffer cmd, VulkanContext &ctx, float dt)
{
    rainModeLastFrame = 0;
    rainInstancesLastFrame = 0;
    rainLevelsLastFrame = 0;

    // The eye, in double (as the rain frame is built, fillCloudsV2Params): its velocity smears the drops it
    // passes. Capped at 30 m/s (WASD near the ground is a few m/s; a flight at 500 km/s would streak every drop
    // across the screen), and a jump (a Go to, a teleport) is no motion.
    const float ground = (terrainFrameMapped && (debugDisableMask & 1024u) == 0) ? terrainFrameMapped[1] : obsTerrainH;
    const glm::dvec3 up = glm::normalize(glm::dvec3(obsDir));
    const double eyeAsl = std::max((double)ground + 2.0, (double)obsHeightOffset);
    const glm::dvec3 eye = up * (6371000.0 + eyeAsl);
    glm::dvec3 vel(0.0);
    if (rainPrevEyeValid && dt > 1e-4f)
    {
        vel = (eye - rainPrevEye) / (double)dt;
        if (glm::length(vel) > 2000.0)
            vel = glm::dvec3(0.0);
    }
    rainPrevEye = eye;
    rainPrevEyeValid = true;

    const bool lightRan = cv2LightningRanThisFrame;
    cv2LightningRanThisFrame = false;
    if (!rainPipeline || !rainDescSet || !lightRan || !cv2FlashMapped || cv2RainStreaks <= 0.0f || eyeAsl > 6000.0)
        return;
    // Is there anything to draw: rain anywhere in the map around the eye, or diamond dust in sunshine (the
    // previous frame's values, read on the host; the shader uses this frame's).
    const float *hdr = (const float *)cv2FlashMapped;
    const float *wgMax = (const float *)((const char *)cv2FlashMapped + kCv2RainMapOffset - 16 - 80);
    float rainNear = 0.0f;
    for (int i = 0; i < 4; ++i)
        rainNear = std::max(rainNear, wgMax[i]);
    const float sunT = hdr[1], iceS = hdr[2];
    int mode = 0;
    if (cv2RainAmount > 0.0f && rainNear > 0.0f)
        mode = 1;
    else if (iceS > 1e-6f && sunT > 0.01f)
        mode = 2;
    if (mode == 0)
        return;

    // Levels: box k is 4 m x 2^k, drawn out to half its side; the reach picks the last (at most 6: 64 m).
    const float reach = std::clamp(cv2DropDistM, 4.0f, 64.0f);
    const int levels = std::clamp((int)std::lround(std::log2(reach / 2.0f)) + 1, 1, 6);
    const uint32_t n0 = (uint32_t)std::clamp(cv2RainParticlesK * 1000.0f, 500.0f, 64000.0f);
    const uint32_t instances = n0 * ((1u << levels) - 1u);

    RainDrawPC pc{};
    pc.skyView = camera.viewMatrix();
    pc.fovYRad = glm::radians(camera.fovYDeg);
    pc.screenW = (float)ctx.swapExtent.width;
    pc.screenH = (float)ctx.swapExtent.height;
    pc.aspect = pc.screenW / pc.screenH;
    {
        const glm::dvec3 E = glm::normalize(glm::cross(glm::dvec3(0.0, 0.0, 1.0), up));
        const glm::dvec3 N = glm::cross(up, E);
        glm::dvec3 v(glm::dot(vel, E), glm::dot(vel, N), glm::dot(vel, up));
        const double sp = glm::length(v);
        if (sp > 30.0)
            v *= 30.0 / sp;
        pc.eyeVel = glm::vec4(glm::vec3(v), std::clamp(cv2RainShutterMs, 1.0f, 100.0f) * 1e-3f);
    }
    pc.params = glm::vec4((float)n0, (float)levels, cv2RainStreaks, skyExposure());
    const bool manualDepth = renderScale < 0.999f && (debugDisableMask & 1024u) == 0u && !skyTaaUsedThisFrame;
    pc.obsECEFDir = glm::vec4(obsDir, (float)((mode == 2 ? 1u : 0u) | (manualDepth ? 2u : 0u)));

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, rainPipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, rainPipeLayout, 0, 1, &rainDescSet, 0, nullptr);
    vkCmdPushConstants(cmd, rainPipeLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                       sizeof(pc), &pc);
    vkCmdDraw(cmd, 4, instances, 0, 0);
    rainModeLastFrame = mode;
    rainInstancesLastFrame = instances;
    rainLevelsLastFrame = levels;
}

// ── The rain's motion (2026-10-05) ─────────────────────────────────────────────────────────────────
// Integrated here over SIM time (so pause stops it and reverse runs it backwards), in double, and handed to the
// shader wrapped to 1024 m (every box divides it): the fall phase (a drop's offset is its terminal speed x this),
// and the wind drift of rain and of snow. Integrating is what lets the speed and the wind CHANGE — with a
// shower's strength, in gusts, with a slider — without moving every drop at once: the first cut (2026-10-04)
// took v x t with t up to 600 s, so the speed had to be a fixed constant per drop and the wind a fixed constant
// per box. The wind: the ground wind ("Rain wind" x 0.4 x the wind aloft, toward the east), plus a shower's
// OUTFLOW ("Storm wind" at rain rate ~0.6 and up), blowing away from the rain map's rain-weighted centre, in
// gusts ("Wind gusts": a smooth noise over ~4 s of sim time, stronger in heavy rain, turning the wind +-15 deg).
// The fall: "Rain fall speed" x the drops' terminal velocities, x 1.35 at rain rate 1 (heavier rain, bigger drops).
void SatelliteSim::updateRainMotion(GpuCloudV2Params &p)
{
    const double simT = (double)simDayJ2000 * 86400.0 + simSecInDay;
    const double dts = rainPrevSimTValid ? simT - rainPrevSimT : 0.0;
    rainPrevSimT = simT;
    rainPrevSimTValid = true;
    const float ease = 1.0f - std::exp(-(float)std::min(std::abs(dts), 5.0) / 0.6f);

    // The previous frame's rain map (cloud_lightning.glsl): the rate at the eye and the rain-weighted centre.
    float rate = 0.0f;
    glm::vec2 centre(0.0f);
    if (cv2FlashMapped && cloudsV2LightningActive())
    {
        const float *map = (const float *)((const char *)cv2FlashMapped + kCv2RainMapOffset);
        const int n = 32, c = n / 2;
        rate = 0.25f * (map[(c - 1) * n + c - 1] + map[(c - 1) * n + c] + map[c * n + c - 1] + map[c * n + c]);
        double sw = 0.0, sx = 0.0, sy = 0.0;
        for (int j = 0; j < n; ++j)
            for (int i = 0; i < n; ++i)
            {
                const double w = map[j * n + i];
                sw += w;
                sx += w * (i - c + 0.5) * 40.0;
                sy += w * (j - c + 0.5) * 40.0;
            }
        if (sw > 1e-4)
            centre = glm::vec2((float)(sx / sw), (float)(sy / sw));
    }
    rainRateEased += (rate - rainRateEased) * ease;
    const float rN = std::clamp(rainRateEased / 0.6f, 0.0f, 1.0f);

    // The wind it falls through.
    const glm::vec2 ambient(0.4f * cv2WindMps * cv2RainWindGain, 0.0f);
    const glm::vec2 ambDir = glm::length(ambient) > 0.01f ? glm::normalize(ambient) : glm::vec2(1.0f, 0.0f);
    const float cl = glm::length(centre);
    const glm::vec2 outDir = glm::normalize(glm::mix(ambDir, cl > 1.0f ? -centre / cl : ambDir,
                                                     glm::smoothstep(40.0f, 160.0f, cl)) + glm::vec2(1e-5f, 0.0f));
    auto vnoise = [](double x, double salt) {
        const double i = std::floor(x), f = x - i;
        auto h = [&](double k) { double s = std::sin(k * 127.1 + salt * 311.7) * 43758.5453; return s - std::floor(s); };
        const double u = f * f * (3.0 - 2.0 * f);
        return (float)(2.0 * (h(i) + (h(i + 1.0) - h(i)) * u) - 1.0);
    };
    const float gust = 0.65f * vnoise(simT / 4.0, 1.0) + 0.35f * vnoise(simT / 1.5, 2.0);
    const float turn = glm::radians(15.0f) * vnoise(simT / 6.0, 3.0) * std::min(cv2RainGusts, 2.0f);
    glm::vec2 target = ambient + outDir * (cv2RainStormWind * rN);
    target *= std::max(1.0f + cv2RainGusts * (0.2f + 0.4f * rN) * gust, 0.0f);
    target = glm::vec2(target.x * std::cos(turn) - target.y * std::sin(turn), target.x * std::sin(turn) + target.y * std::cos(turn));
    rainWindNow += (target - rainWindNow) * ease;

    rainFallRefNow = std::max(cv2RainFallSpeed, 0.05f) * (1.0f + 0.35f * rN);
    rainFallPhase += rainFallRefNow * dts;
    rainDrift += glm::dvec2(rainWindNow) * dts;
    snowDrift += glm::dvec2(rainWindNow) * (double)std::clamp(cv2SnowWind, 0.0f, 8.0f) * dts;

    auto wrap = [](double v) { v = std::fmod(v, 1024.0); return (float)(v < 0.0 ? v + 1024.0 : v); };
    p.rainMotion = glm::vec4(wrap(rainFallPhase / 64.0), rainFallRefNow, wrap(rainDrift.x), wrap(rainDrift.y));
    p.rainWind = glm::vec4(rainWindNow, wrap(snowDrift.x), wrap(snowDrift.y));
}
