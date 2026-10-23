// Rain, snow and diamond dust at the eye as PARTICLES (2026-10-04).
//
// One instanced draw in the main pass, after the sky TAA has resolved the frame (the drops are crisp, full
// resolution, and never smeared by the history): rain_particles.vert builds every drop from its instance index
// (nested world-fixed boxes about the eye, Marshall-Palmer sizes, terminal velocities, the rain map around the
// eye deciding which exist), draws it as the streak it sweeps over the shutter time with its real coverage, and
// lights it with the cloud march's rain sample at the eye (the lightning pass writes cv2RainKey / cv2RainAmb)
// through the rain's own phase function — the bows included (include/cv2_optics.glsl). The particles are the
// near part of the rain volume, drawn one drop at a time. It replaced cloud_march.comp's rainDrops.
#include "../FrameProfile.h"
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
    {
        // The drops' Marshall-Palmer slope (1/mm) for the rain falling round the eye: the rate at the eye, or most of
        // the nearest shaft's when the eye is at its edge (R = 60 rate^1.5 mm/h, as the shader's cross-section).
        const float rs = std::max(std::max(rainRateEased, 0.7f * rainNear), 1e-3f);
        const float R = std::max(60.0f * std::pow(rs, 1.5f), 0.5f);
        pc.dropLambda = 4.1f * std::pow(R, -0.21f);
    }
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

// ── The rain's motion (2026-10-05; the wind reworked 2026-10-23) ─────────────────────────────────────
// Integrated here over SIM time (so pause stops it and reverse runs it backwards), in double, and handed to the
// shader wrapped to 1024 m (every box divides it): the fall phase (a drop's offset is its terminal speed x this),
// and the wind drift of rain and of snow. Integrating is what lets the speed and the wind CHANGE — with a
// shower's strength, in gusts, with a slider — without moving every drop at once.
//
// The wind (surfaceWindAt, 2026-10-23) is the SURFACE wind at the eye, a pure function of place and sim time:
//   - a mean wind from a slow value noise over place (~500 km) and hours, at the eye's height: median ~1.4 m/s, up
//     to ~6, any direction ("Surface wind (x)"). It was 0.4 x the cloud wind aloft, always toward the east, plus a shower's outflow at
//     9.5 m/s x the rain rate from ANY shower, snow included (x 1.2 for snow): flakes falling ~1.5 m/s in a 5-6 m/s
//     wind travel at ~75 deg from the vertical, "blowing straight sideways";
//   - the storm towers' OUTFLOW ("Storm outflow (m/s)"): away from each tower within ~60 km (wx::Catalog's towers,
//     read only), strongest a few km out, by its strength (a flanking tower half);
//   - GUST FRONTS ("Gust fronts"): events hashed per 15-s bin of sim time (and 2-degree cell), each a 1-2 s rise to
//     +50-100% of the wind (+ an absolute part, larger near storms and in heavy rain; at most +10 m/s), turned 15-40
//     deg, decaying over 5-20 s. Their rate grows with the wind, the rain and the storms nearby; a bin's chance fades in with the rate
//     (smoothstep on its hash), so a change of conditions never pops a gust in mid-decay;
//   - small continuous turbulence ("Wind gusts").
// The rain map's shower outflow (away from its rain-weighted centre, x the rate) stays, at 0.25 x the storm outflow
// and 0.3 of that for snow. The result is eased over ~0.25 s.
namespace {
uint32_t rnHash(uint32_t x)
{
    x ^= x >> 16; x *= 0x7feb352du; x ^= x >> 15; x *= 0x846ca68bu; x ^= x >> 16;
    return x;
}
double rnU(uint64_t a, uint32_t salt)
{
    const uint32_t h = rnHash((uint32_t)a ^ rnHash((uint32_t)(a >> 32) + 0x632BE5ABu) ^ rnHash(salt * 0x9E3779B9u));
    return (double)(h >> 8) * (1.0 / 16777216.0);
}
// Trilinear value noise in [0, 1] with a smoothstep blend (deterministic, no tables).
float rnNoise3(double x, double y, double z, uint32_t salt)
{
    const double fx = std::floor(x), fy = std::floor(y), fz = std::floor(z);
    const double tx = x - fx, ty = y - fy, tz = z - fz;
    const double ux = tx * tx * (3.0 - 2.0 * tx), uy = ty * ty * (3.0 - 2.0 * ty), uz = tz * tz * (3.0 - 2.0 * tz);
    auto h = [&](int dx, int dy, int dz) {
        const uint64_t k = ((uint64_t)(uint32_t)((int64_t)fx + dx) * 73856093ull)
                         ^ ((uint64_t)(uint32_t)((int64_t)fy + dy) * 19349663ull << 11)
                         ^ ((uint64_t)(uint32_t)((int64_t)fz + dz) * 83492791ull << 22);
        return rnU(k, salt);
    };
    auto L = [](double a, double b, double t) { return a + (b - a) * t; };
    const double x00 = L(h(0, 0, 0), h(1, 0, 0), ux), x10 = L(h(0, 1, 0), h(1, 1, 0), ux);
    const double x01 = L(h(0, 0, 1), h(1, 0, 1), ux), x11 = L(h(0, 1, 1), h(1, 1, 1), ux);
    return (float)L(L(x00, x10, uy), L(x01, x11, uy), uz);
}
float rnSmooth(float a, float b, float x)
{
    const float t = std::clamp((x - a) / (b - a), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}
}

SatelliteSim::SurfaceWind SatelliteSim::surfaceWindAt(double simT, float rN)
{
    SurfaceWind w;
    const glm::dvec3 up = glm::normalize(glm::dvec3(obsDir));
    const glm::dvec3 E = glm::normalize(glm::cross(glm::dvec3(0.0, 0.0, 1.0), up) + glm::dvec3(1e-12, 0.0, 0.0));
    const glm::dvec3 N = glm::cross(up, E);
    const double tH = simT / 3600.0;

    // The mean wind: a slow noise over place and hours. nS^2 skews it light (median ~2 m/s near the ground).
    const glm::dvec3 p = up * 12.0;
    const float nS = std::clamp(0.7f * rnNoise3(p.x, p.y, p.z + tH / 4.0, 11u)
                              + 0.3f * rnNoise3(p.x * 2.7 + 5.0, p.y * 2.7, p.z * 2.7 + tH / 1.3, 12u), 0.0f, 1.0f);
    // At the eye's height (~2 m, under the 10-m wind) and skewed light: median ~1.4 m/s, up to ~6.
    const float speed = std::max(cv2RainWindGain, 0.0f) * (0.4f + 5.6f * std::pow(nS, 2.5f));
    const float ang = 4.0f * glm::pi<float>() * rnNoise3(p.x * 0.6 + 11.0, p.y * 0.6, p.z * 0.6 + tH / 7.0, 13u);
    w.base = speed * glm::vec2(std::cos(ang), std::sin(ang));

    // The storm towers' outflow (map frame -> Earth-fixed as stormNearestJson).
    if (stormObjects && !stormTowers.empty() && cv2RainStormWind > 0.0f)
    {
        const double ph = cloudDriftPhase(), c = std::cos(ph), s = std::sin(ph);
        const double kR = 6371000.0, reach = 60000.0, cosReach = std::cos(reach / kR);
        glm::dvec2 out(0.0);
        for (const wx::Tower &tw : stormTowers)
        {
            const glm::dvec3 d = glm::normalize(tw.dir);
            const glm::dvec3 e(d.x * c + d.y * s, -d.x * s + d.y * c, d.z);
            const double ce = glm::dot(e, up);
            if (ce < cosReach) continue;
            const double dist = std::acos(std::clamp(ce, -1.0, 1.0)) * kR;
            const double wT = (double)tw.strength * (tw.role >= 0.99f ? 1.0 : 0.5);
            w.storm += (float)(wT * std::exp(-dist / 15000.0));
            const glm::dvec3 to = e - up * ce;
            const glm::dvec2 toEN(glm::dot(to, E), glm::dot(to, N));
            const double l = glm::length(toEN);
            if (l < 1e-12) continue;
            const double prof = (double)rnSmooth(300.0f, 3000.0f, (float)dist) * std::exp(-std::max(dist - 5000.0, 0.0) / 12000.0);
            out -= toEN / l * ((double)cv2RainStormWind * wT * prof);
        }
        const double ol = glm::length(out), cap = 1.5 * (double)cv2RainStormWind;
        if (ol > cap) out *= cap / ol;
        w.outflow = glm::vec2(out);
        w.storm = std::min(w.storm, 1.5f);
    }

    // Gust fronts.
    const glm::vec2 mean = w.base + w.outflow;
    const float meanL = glm::length(mean);
    const glm::vec2 dirRef = meanL > 0.3f ? mean / meanL : glm::vec2(std::cos(ang), std::sin(ang));
    glm::vec2 gustV(0.0f);
    const float fronts = std::max(cv2GustFronts, 0.0f);
    if (fronts > 0.0f)
    {
        const double latD = glm::degrees(std::asin(std::clamp(up.z, -1.0, 1.0))), lonD = glm::degrees(std::atan2(up.y, up.x));
        const uint64_t cell = ((uint64_t)(uint32_t)(int32_t)std::floor(latD / 2.0) << 32) ^ (uint32_t)(int32_t)std::floor(lonD / 2.0);
        // A bin's chance: ~0.1 in calm, dry air (a gust every ~2.5 min) to ~0.45 in a downpour by a storm (~30 s apart).
        const float pRate = std::clamp((0.06f + 0.12f * nS + 0.2f * rN + 0.4f * std::min(w.storm, 1.0f)) * fronts, 0.0f, 0.9f);
        const double B = 15.0;
        const int64_t i0 = (int64_t)std::floor(simT / B);
        for (int k = 0; k < 9; ++k)
        {
            const uint64_t i = (uint64_t)(i0 - k) ^ (cell * 0x9E3779B97F4A7C15ull);
            const float u = (float)rnU(i, 1u);
            const float amp = rnSmooth(u - 0.06f, u + 0.06f, pRate);
            if (amp <= 0.0f) continue;
            const double tau = simT - ((double)(i0 - k) + rnU(i, 2u)) * B;
            if (tau < 0.0) continue;
            const double R = 1.0 + rnU(i, 3u), D = 5.0 + 15.0 * rnU(i, 4u);
            const float env = rnSmooth(0.0f, (float)R, (float)tau) * (float)std::exp(-std::max(tau - R, 0.0) / D);
            if (env < 1e-3f) continue;
            const float rel = 0.5f + 0.5f * (float)rnU(i, 5u);
            const float add = (1.0f + 3.0f * std::min(w.storm, 1.0f) + 1.5f * rN) * (float)rnU(i, 6u) * std::min(fronts, 1.5f);
            const float mag = std::min(rel * meanL + add, 10.0f);   // a strong gust front adds ~10 m/s at most
            const float sw = glm::radians(15.0f + 25.0f * (float)rnU(i, 7u)) * (rnU(i, 8u) < 0.5 ? -1.0f : 1.0f);
            const glm::vec2 dir(dirRef.x * std::cos(sw) - dirRef.y * std::sin(sw), dirRef.x * std::sin(sw) + dirRef.y * std::cos(sw));
            gustV += dir * (mag * env * amp);
            w.gust += env * amp * (rel + add / 6.0f);
            ++w.gustsActive;
        }
    }

    // Small continuous turbulence.
    const float g = std::max(cv2RainGusts, 0.0f);
    const float turb = 1.0f + g * (0.1f + 0.15f * rN) * (2.0f * rnNoise3(simT / 3.0, 0.5, 0.5, 21u) - 1.0f);
    const float turn = glm::radians(8.0f) * g * (2.0f * rnNoise3(simT / 5.0, 0.5, 0.5, 22u) - 1.0f);
    const glm::vec2 v = (mean + gustV) * std::max(turb, 0.0f);
    w.v = glm::vec2(v.x * std::cos(turn) - v.y * std::sin(turn), v.x * std::sin(turn) + v.y * std::cos(turn));
    return w;
}

void SatelliteSim::updateRainMotion(GpuCloudV2Params &p)
{
    FPROF_SCOPE("updateRainMotion");
    const double simT = (double)simDayJ2000 * 86400.0 + simSecInDay;
    const double dts = rainPrevSimTValid ? simT - rainPrevSimT : 0.0;
    rainPrevSimT = simT;
    rainPrevSimTValid = true;
    const float easeRate = 1.0f - std::exp(-(float)std::min(std::abs(dts), 5.0) / 0.6f);
    const float easeWind = 1.0f - std::exp(-(float)std::min(std::abs(dts), 5.0) / 0.25f);

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
        if (!std::isfinite(rate)) rate = 0.0f;
    }
    rainRateEased += (rate - rainRateEased) * easeRate;
    const float rN = std::clamp(rainRateEased / 0.6f, 0.0f, 1.0f);

    // The wind it falls through: the surface wind (gusts, storm outflow) + the shower's own outflow.
    surfaceWindNow_ = surfaceWindAt(simT, rN);
    surfaceWindSimT_ = simT;
    const glm::vec2 meanW = surfaceWindNow_.base + surfaceWindNow_.outflow;
    const glm::vec2 ambDir = glm::length(meanW) > 0.01f ? glm::normalize(meanW) : glm::vec2(1.0f, 0.0f);
    const float cl = glm::length(centre);
    const glm::vec2 outDir = glm::normalize(glm::mix(ambDir, cl > 1.0f ? -centre / cl : ambDir,
                                                     glm::smoothstep(40.0f, 160.0f, cl)) + glm::vec2(1e-5f, 0.0f));
    const float liquid = glm::smoothstep(-1.0f, 3.0f, cv2EyeTempC);
    const glm::vec2 target = surfaceWindNow_.v + outDir * (0.25f * cv2RainStormWind * rN * (0.3f + 0.7f * liquid));
    rainWindNow += (target - rainWindNow) * easeWind;
    if (!std::isfinite(rainWindNow.x) || !std::isfinite(rainWindNow.y))
        rainWindNow = glm::vec2(0.0f);

    // The fall: physical terminal speeds x "Drop fall speed" (1). Heavier rain falls faster through its bigger drops
    // (the shader's sizes follow the rate, pc.dropLambda), not a factor on every drop.
    rainFallRefNow = std::max(cv2RainFallSpeed, 0.05f);
    rainFallPhase += rainFallRefNow * dts;
    // Snow falls at its own speed: "Rain fall speed" up to 1.5x only (flakes ~1 m/s), no faster in heavy precipitation.
    const float snowFallRef = std::clamp(cv2RainFallSpeed, 0.05f, 1.5f);
    snowFallPhase += snowFallRef * dts;
    rainDrift += glm::dvec2(rainWindNow) * dts;
    snowDrift += glm::dvec2(rainWindNow) * (double)std::clamp(cv2SnowWind, 0.0f, 8.0f) * dts;

    auto wrap = [](double v) { v = std::fmod(v, 1024.0); return (float)(v < 0.0 ? v + 1024.0 : v); };
    p.rainMotion = glm::vec4(wrap(rainFallPhase / 64.0), rainFallRefNow, wrap(rainDrift.x), wrap(rainDrift.y));
    p.rainWind = glm::vec4(rainWindNow, wrap(snowDrift.x), wrap(snowDrift.y));
    // zw (2026-10-23): the rain drops' redraw epochs. Each particle stands for tens (near) to thousands (far) of real
    // drops; kept as one body it drifts across the screen as a lump, a few px a frame in the far levels: the "floaty"
    // rain. Redrawn every "Drop persistence" of sim time (each at its own phase, fading in and out), the rain is what a
    // camera sees: unrelated streaks in every frame, their direction and length the motion. z = sim time /
    // persistence mod 4096, w = 1 when on.
    const double te = std::max((double)cv2DropPersistMs, 0.0) * 1e-3;
    double ep = te > 0.0 ? std::fmod(simT / te, 4096.0) : 0.0;
    if (ep < 0.0) ep += 4096.0;
    p.snowMotion = glm::vec4(wrap(snowFallPhase / 64.0), snowFallRef, (float)ep, te > 0.0 ? 1.0f : 0.0f);
}

// The surface wind for the ambience (computeAmbienceContext): this frame's from updateRainMotion, or computed here
// when the clouds did not run (Potato, a knockout).
SatelliteSim::SurfaceWind SatelliteSim::surfaceWindForAmbience()
{
    const double simT = (double)simDayJ2000 * 86400.0 + simSecInDay;
    if (std::abs(surfaceWindSimT_ - simT) > 1.0)
    {
        surfaceWindNow_ = surfaceWindAt(simT, std::clamp(rainRateEased / 0.6f, 0.0f, 1.0f));
        surfaceWindSimT_ = simT;
    }
    return surfaceWindNow_;
}
