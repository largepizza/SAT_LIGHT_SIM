// The turbulence warp (2026-10-28, issues 20261028_193115 "Detail Stars should flicker with turbulence" and
// 20261028_191408 "Star Twinkle"). Atmospheric seeing as a distortion of the WHOLE picture: after the main pass (sky,
// Milky Way and its star-cloud points, Moon, Sun, stars, planets, satellites, meshes) and before the UI, the composed
// frame is copied and resampled through a smooth displacement field (shaders/turb_warp.frag). The sky shader does not
// grow: the Sun's and the Moon's disc jitter moved here (sat_sky.frag no longer jitters them), and star_point.vert no
// longer dances each star on its own.
//
// The field's rms per axis at a pixel = the refraction table's angle-of-arrival rms at the pixel's apparent elevation
// (Refraction.cpp, 2.91 D^-1/3 int Cn^2 ds, D 1 cm; Cn^2 x "Turbulence strength") x the share of the turbulent column in
// front of what the pixel shows (scene depth, cloud composite) x a fade above the air. In pixels it is that angle / the
// pixel's angle: invisible at a wide field (a 60-deg view's pixel is ~1 mrad; the jitter is 1-50 urad), boiling when
// zoomed. When the largest displacement in view is under 0.3 px (3 sigma) the pass does not run at all (no copy).
//
// The eddy band (2026-11-03, issue 20261103_191106 "Turbulence": the field was one ~12"-or-6-px cell, grown to 4x the
// largest displacement so it would not fold, which at the user's 5.5x read as big chunky wobbles). A Kolmogorov tilt
// field has roughly equal variance per octave of eddy size from the aperture D (1 cm) to the outer scale L0 (~5 m near
// the ground, 0.4 x the height of the air, 1-50 m); seen through air whose turbulence sits aoaS away (the Cn^2-weighted
// mean distance) that is the angular band D / aoaS .. L0 / aoaS (the sea 4.7 km out from 14 m: 0.4" .. 4'; a sky ray at
// the horizon, aoaS ~95 km at ~700 m up: 0.02" .. 1.8'), x "Turbulence cell size". The field is six octaves of
// vector value noise on fixed absolute angular cells (1 urad x 2^k): those from ~1.5 px up carry the band's variance as
// displacement (each capped at 1/8 of its cell, so it never folds), the band below them (sub-pixel eddies, the seeing
// disc) is a blur. So a sea-level horizon at 10"/px shimmers by ~half a pixel and softens; the chunks are gone.
//
// Time and place (same issue): the field evolves on SIM time (paused = frozen; turbClock_ = sim seconds x "Turbulence
// speed"), and each octave is a frozen 3D field in the air, its lattice in ECEF metres on a sphere L_k = 100 / 300 /
// 900 m around the eye (cell l_k = theta_k L_k): turning the camera keeps it fixed on the scene, moving the eye by
// ~l_k changes it (a metre re-draws every octave: two eyes a metre apart see independent image motion). The lattice
// drifts through itself at ~0.8 cells / s (a wind: frozen flow across the view, boiling along it); the physical
// crossing rates v / l are 10-100 Hz at these scales, above the frame rate, which is what the blur stands for.
#include "SatelliteSim.h"
#include "Refraction.h"
#include "FrameProfile.h"
#include "Log.h"
#include <cmath>
#include <algorithm>

namespace
{
constexpr int kTabN = 64;
constexpr int kOct = 6;                  // octaves evaluated per pixel
constexpr double kThetaBase = 1e-6;      // octave k's angular cell: kThetaBase x 2^k (rad)
constexpr double kApertureM = 0.01;      // the band's inner end: the aperture the table's aoa is for (Refraction.cpp)
// Its outer end: the outer scale grows with the height of the turbulent air, L0 = 0.4 z (the surface layer's mixing
// length), 1-50 m (site measurements in the free air: ~20 m median, Paranal's 22 m); z is where the ray is aoaS out.
constexpr double kOuterPerZ = 0.4, kOuterMinM = 1.0, kOuterMaxM = 50.0;
constexpr double kCellsPerS = 0.8;       // the frozen flow's drift (cells / s at speed 1)
constexpr double kFadeLoPx = 1.5, kFadeHiPx = 3.0;   // the finest octave fades in over these cell sizes ...
constexpr double kTopLoPx = 48.0, kTopHiPx = 96.0;   // ... and the coarsest out over these
constexpr float kFieldRms = 0.40f;       // the quintic value noise of uniform [-1, 1] corners: rms per component
// The pass runs only when the largest displacement on screen (3 sigma) reaches this: below ~0.1 px rms the motion is
// invisible and the resampling would only cost (~0.27 ms at 1920x1009) and soften the stars a little.
constexpr float kMinRunPx = 0.3f;

uint32_t hashU(uint32_t x)
{
    x ^= x >> 16; x *= 0x7feb352dU; x ^= x >> 15; x *= 0x846ca68bU; x ^= x >> 16;
    return x;
}
double hash01(uint32_t a, uint32_t b) { return hashU(a * 0x9E3779B1u ^ hashU(b + 0x85EBCA77u)) / 4294967296.0; }
} // namespace

void SatelliteSim::createTurbWarp(VulkanContext &ctx)
{
    turbMapped = createMirrored(ctx, sizeof(GpuTurbWarpUBO), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, turbBuf, turbMem, true,
                                "turbulence warp");
    VkDescriptorSetLayoutBinding bnd[5] = {};
    bnd[0] = {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    for (int i = 1; i < 5; ++i)
        bnd[i] = {(uint32_t)i, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    li.bindingCount = 5;
    li.pBindings = bnd;
    vkCreateDescriptorSetLayout(ctx.device, &li, nullptr, &turbLayout);
    VkDescriptorPoolSize ps[2] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1}, {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 4}};
    VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pi.poolSizeCount = 2;
    pi.pPoolSizes = ps;
    pi.maxSets = 1;
    vkCreateDescriptorPool(ctx.device, &pi, nullptr, &turbPool);
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = turbPool;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &turbLayout;
    vkAllocateDescriptorSets(ctx.device, &ai, &turbSet);
    VkDescriptorBufferInfo ubInfo{turbBuf, 0, VK_WHOLE_SIZE};
    VkWriteDescriptorSet wr{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, turbSet, 0, 0, 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, nullptr, &ubInfo, nullptr};
    frameSafeUpdateDescriptorSets(ctx.device, 1, &wr, 0, nullptr);

    VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sci.magFilter = VK_FILTER_LINEAR;
    sci.minFilter = VK_FILTER_LINEAR;
    sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    vkCreateSampler(ctx.device, &sci, nullptr, &turbFrameSampler);

    VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &turbLayout;
    vkCreatePipelineLayout(ctx.device, &pli, nullptr, &turbPipeLayout);

    VkShaderModule vert = ctx.loadShader("shaders/taa_fullscreen.vert.spv");
    VkShaderModule frag = ctx.loadShader("shaders/turb_warp.frag.spv");
    VkPipelineShaderStageCreateInfo stages[2] = {};
    stages[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, vert, "main", nullptr};
    stages[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, frag, "main", nullptr};
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vps{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vps.viewportCount = 1;
    vps.scissorCount = 1;
    VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dsi{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dsi.dynamicStateCount = 2;
    dsi.pDynamicStates = dyn;
    VkPipelineRasterizationStateCreateInfo rast{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rast.polygonMode = VK_POLYGON_MODE_FILL;
    rast.cullMode = VK_CULL_MODE_NONE;
    rast.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rast.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    ds.depthTestEnable = VK_FALSE;
    ds.depthWriteEnable = VK_FALSE;
    VkPipelineColorBlendAttachmentState cba{};
    cba.blendEnable = VK_FALSE;
    cba.colorWriteMask = 0xF;
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
    ci.pDynamicState = &dsi;
    ci.layout = turbPipeLayout;
    ci.renderPass = ctx.renderPass;   // compatible with renderPassLoad, which it is drawn in
    ci.subpass = 0;
    if (vkCreateGraphicsPipelines(ctx.device, ctx.pipelineCache, 1, &ci, nullptr, &turbPipeline) != VK_SUCCESS)
        throw std::runtime_error("SatelliteSim: failed to create the turbulence warp pipeline");
    vkDestroyShaderModule(ctx.device, vert, nullptr);
    vkDestroyShaderModule(ctx.device, frag, nullptr);
    createTurbWarpFrame(ctx);
    writeTurbWarpTargets(ctx);
}

// The frame copy, at the swapchain's (or the HQ photo target's) extent.
void SatelliteSim::createTurbWarpFrame(VulkanContext &ctx)
{
    if (!turbSet)
        return;
    if (turbFrameImg && turbFrameExtent.width == ctx.swapExtent.width && turbFrameExtent.height == ctx.swapExtent.height)
        return;
    if (turbFrameImg)
    {
        ctx.drainFrames();
        vkDestroyImageView(ctx.device, turbFrameView, nullptr);
        vkDestroyImage(ctx.device, turbFrameImg, nullptr);
        vkFreeMemory(ctx.device, turbFrameMem, nullptr);
    }
    turbFrameExtent = ctx.swapExtent;
    ctx.createImage(ctx.swapExtent.width, ctx.swapExtent.height, ctx.swapFormat,
                    VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, turbFrameImg, turbFrameMem);
    VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vci.image = turbFrameImg;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format = ctx.swapFormat;
    vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCreateImageView(ctx.device, &vci, nullptr, &turbFrameView);
    // Into SHADER_READ_ONLY once, so every frame's copy starts from a known layout.
    VkCommandBuffer c = ctx.beginOneTimeCommands();
    ctx.imageBarrier(c, turbFrameImg, 0, VK_ACCESS_SHADER_READ_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                     VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    ctx.endOneTimeCommands(c);
    VkDescriptorImageInfo fInfo{turbFrameSampler, turbFrameView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet wr{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, turbSet, 1, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &fInfo, nullptr, nullptr};
    frameSafeUpdateDescriptorSets(ctx.device, 1, &wr, 0, nullptr);
}

void SatelliteSim::writeTurbWarpTargets(VulkanContext &ctx)
{
    if (!turbSet || !cloudMarchTargetAView || !sceneDepthView)
        return;
    VkDescriptorImageInfo aInfo{cloudMarchSampler, cloudMarchTargetAView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkDescriptorImageInfo bInfo{cloudMarchSampler, cloudMarchTargetBView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkDescriptorImageInfo dInfo{sceneDepthSampler, sceneDepthView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet wr[3] = {};
    wr[0] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, turbSet, 2, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &aInfo, nullptr, nullptr};
    wr[1] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, turbSet, 3, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &bInfo, nullptr, nullptr};
    wr[2] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, turbSet, 4, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &dInfo, nullptr, nullptr};
    frameSafeUpdateDescriptorSets(ctx.device, 3, wr, 0, nullptr);
}

void SatelliteSim::destroyTurbWarp(VkDevice device)
{
    if (turbPipeline) vkDestroyPipeline(device, turbPipeline, nullptr);
    if (turbPipeLayout) vkDestroyPipelineLayout(device, turbPipeLayout, nullptr);
    if (turbPool) vkDestroyDescriptorPool(device, turbPool, nullptr);
    if (turbLayout) vkDestroyDescriptorSetLayout(device, turbLayout, nullptr);
    if (turbBuf) vkDestroyBuffer(device, turbBuf, nullptr);
    if (turbMem) vkFreeMemory(device, turbMem, nullptr);
    if (turbFrameSampler) vkDestroySampler(device, turbFrameSampler, nullptr);
    if (turbFrameView) vkDestroyImageView(device, turbFrameView, nullptr);
    if (turbFrameImg) vkDestroyImage(device, turbFrameImg, nullptr);
    if (turbFrameMem) vkFreeMemory(device, turbFrameMem, nullptr);
    turbPipeline = VK_NULL_HANDLE;
    turbPipeLayout = VK_NULL_HANDLE;
    turbPool = VK_NULL_HANDLE;
    turbLayout = VK_NULL_HANDLE;
    turbSet = VK_NULL_HANDLE;
    turbBuf = VK_NULL_HANDLE;
    turbMem = VK_NULL_HANDLE;
    turbMapped = nullptr;
    turbFrameSampler = VK_NULL_HANDLE;
    turbFrameView = VK_NULL_HANDLE;
    turbFrameImg = VK_NULL_HANDLE;
    turbFrameMem = VK_NULL_HANDLE;
    turbFrameExtent = {0, 0};
}

// Every frame from recordDraw: the UBO and whether the pass runs.
void SatelliteSim::updateTurbWarp(VulkanContext &ctx)
{
    FPROF_SCOPE("turbWarp.update");
    turbWarpActive_ = false;
    turbMaxPx_ = 0.0f;
    const refr::Table &t = refrTable;
    if (!turbMapped || !turbPipeline || !t.valid || (debugDisableMask & 262144u))
        return;
    const double h0 = (double)obsEyeRadiusM(obsHeightOffset) - (double)satphot::kEarthRadiusM;
    // Above the air: the turbulent column is far and thin; faded out from 30 to 80 km.
    const double hf = std::clamp((h0 - 30000.0) / 50000.0, 0.0, 1.0);
    turbAltFade_ = (float)(1.0 - hf * hf * (3.0 - 2.0 * hf));
    if (turbAltFade_ <= 0.0f)
        return;
    const VkExtent2D ex = ctx.swapExtent;
    if (ex.width == 0 || ex.height == 0 || ex.width != turbFrameExtent.width || ex.height != turbFrameExtent.height)
        return;
    const glm::mat4 V = camera.viewMatrix();
    const glm::vec3 camR(V[0][0], V[1][0], V[2][0]);
    const glm::vec3 camU(V[0][1], V[1][1], V[2][1]);
    const glm::vec3 camF = -glm::vec3(V[0][2], V[1][2], V[2][2]);
    const double fovY = glm::radians((double)camera.fovYDeg);
    const double tanH = std::tan(0.5 * fovY);
    const double aspect = (double)ex.width / (double)ex.height;
    const double pixAng = 2.0 * tanH / (double)ex.height;   // rad / px at the centre

    // The aoa table over apparent elevation, denser toward the horizon: e = eLo + span u^2.
    const double r0 = satphot::kEarthRadiusM + std::max(h0, 0.0);
    const double eLimb = -std::acos(std::min(satphot::kEarthRadiusM / r0, 1.0));
    const double eLo = eLimb - 0.01, span = 0.5 * glm::pi<double>() - eLo;
    auto *u = static_cast<GpuTurbWarpUBO *>(turbMapped);
    // The view's elevation range (for the largest displacement on screen).
    const double halfDiag = std::atan(tanH * std::sqrt(1.0 + aspect * aspect));
    const double eCam = std::asin(std::clamp((double)camF.z, -1.0, 1.0));
    const double eMin = eCam - halfDiag, eMax = eCam + halfDiag;
    double aoaMax = 0.0;
    for (int i = 0; i < kTabN; ++i)
    {
        const double x = (double)i / (kTabN - 1);
        const double e = eLo + span * x * x;
        refr::Entry en;
        double dRg;
        refr::lookup(t, e, dRg, &en);
        const float aoa = std::isfinite(en.aoa) ? en.aoa : 0.0f;
        const float s = std::isfinite(en.aoaS) ? en.aoaS : 0.0f;
        u->tab[i] = glm::vec4(aoa, s, 0.0f, 0.0f);
        const double eN = eLo + span * std::pow((double)std::min(i + 1, kTabN - 1) / (kTabN - 1), 2.0);
        if (eN >= eMin && e <= eMax)
            aoaMax = std::max(aoaMax, (double)aoa);
    }
    const double gain = (double)turbAltFade_;
    // Largest displacement on screen: ~3 sigma of the field in its stronger (vertical) axis.
    turbMaxPx_ = (float)(3.0 * 1.2 * aoaMax * gain / pixAng);
    if (turbMaxPx_ < kMinRunPx)
        return;

    // ECEF: the eye (double) and its ENU frame; the camera's axes in it.
    const glm::dvec3 eye = followActive ? followObsEcef : glm::normalize(glm::dvec3(obsDir)) * (double)obsEyeRadiusM();
    const glm::dvec3 up = glm::normalize(eye);
    glm::dvec3 east = glm::cross(glm::dvec3(0.0, 0.0, 1.0), up);
    east = glm::length(east) > 1e-9 ? glm::normalize(east) : glm::dvec3(1.0, 0.0, 0.0);
    const glm::dvec3 north = glm::cross(up, east);
    auto toEcef = [&](const glm::vec3 &v) { return east * (double)v.x + north * (double)v.y + up * (double)v.z; };
    const glm::dvec3 R = toEcef(camR), U = toEcef(camU), F = toEcef(camF);

    // The octaves: the finest whose cell is >= kFadeLoPx at the centre, and five coarser. Each octave's level k is
    // absolute (its cell and seed never change with the zoom: the set slides, fading in at the fine end and out at the
    // coarse end).
    const double xl = std::log2(kFadeLoPx * pixAng / kThetaBase);
    const int kLo = (int)std::ceil(xl);
    turbOctLoPx_ = (float)(kThetaBase * std::exp2((double)kLo) / pixAng);
    static const double kLayerM[3] = {100.0, 300.0, 900.0};
    auto wrapD = [](double v) { return v - 4096.0 * std::floor(v / 4096.0); };
    for (int o = 0; o < kOct; ++o)
    {
        const int k = kLo + o;
        const double th = kThetaBase * std::exp2((double)k);
        const double cPx = th / pixAng;
        double fade = 1.0;
        if (o == 0)
        {
            const double t = std::clamp((cPx - kFadeLoPx) / (kFadeHiPx - kFadeLoPx), 0.0, 1.0);
            fade = t * t * (3.0 - 2.0 * t);
        }
        if (o == kOct - 1)
        {
            const double t = std::clamp((cPx - kTopLoPx) / (kTopHiPx - kTopLoPx), 0.0, 1.0);
            fade = 1.0 - t * t * (3.0 - 2.0 * t);
        }
        const uint32_t kk = (uint32_t)(k + 64);
        const double Lk = kLayerM[kk % 3u];
        const double ell = th * Lk;   // the cell in metres at the layer's distance
        // The wind: a hashed direction per octave (its component along the view boils, across it drifts).
        glm::dvec3 wd(hash01(kk, 7u) - 0.5, hash01(kk, 8u) - 0.5, hash01(kk, 9u) - 0.5);
        wd = glm::length(wd) > 1e-6 ? glm::normalize(wd) : glm::dvec3(1.0, 0.0, 0.0);
        const double drift = std::fmod(kCellsPerS * turbClock_, 4096.0 * 64.0);
        // The forward ray's point on the layer, in cells, wrapped mod 4096 in double before it reaches float.
        glm::dvec3 q = eye / ell + F / th + wd * drift;
        q = glm::dvec3(wrapD(q.x), wrapD(q.y), wrapD(q.z));
        const glm::dvec3 fl = glm::floor(q);
        u->anc[o] = glm::ivec4((int)fl.x & 4095, (int)fl.y & 4095, (int)fl.z & 4095, (int)kk);
        const glm::dvec3 fr = q - fl;
        u->frac[o] = glm::vec4((float)fr.x, (float)fr.y, (float)fr.z, (float)(1.0 / th));
        u->oct[o] = glm::vec4((float)std::log2(th), (float)fade, (float)cPx, 0.0f);
    }
    u->camR = glm::vec4(glm::vec3(R), 0.0f);
    u->camU = glm::vec4(glm::vec3(U), 0.0f);
    u->camF = glm::vec4(glm::vec3(F), 0.0f);
    u->up = glm::vec4(glm::vec3(up), 0.0f);
    u->screen = glm::vec4((float)ex.width, (float)ex.height, (float)(tanH * aspect), (float)tanH);
    u->params = glm::vec4((float)pixAng, (float)gain, 1.0f / kFieldRms, 1.0f / (kTabN - 1));
    // The band (log2 of metres; the shader subtracts log2 aoaS to make it angles): the aperture and the outer scale x
    // "cell size", and the angle where the displayed octaves take over from the blur (the middle of the finest's fade).
    const double sz = std::clamp((double)turbCellSize, 0.25, 4.0);
    u->band = glm::vec4((float)std::log2(kApertureM * sz), (float)std::log2(sz),
                        (float)std::log2(0.5 * (kFadeLoPx + kFadeHiPx) * pixAng), (float)std::max(h0, 0.0));
    u->outer = glm::vec4((float)kOuterPerZ, (float)kOuterMinM, (float)kOuterMaxM, 0.0f);
    // Near the horizon the jitter is mostly vertical (the old disc jitter's 0.9 / 1.2), isotropic from ~10 deg up
    // (the camera's elevation: uniform over the frame).
    const double hz = std::clamp((eCam - 0.03) / 0.15, 0.0, 1.0);
    const float ah = (float)(0.9 + 0.1 * hz), av = (float)(1.2 - 0.2 * hz);
    u->aniso = glm::vec4(ah, av, (float)eLo, (float)span);
    turbWarpActive_ = true;
}

// From recordPreUiCopy, the image in TRANSFER_SRC_OPTIMAL after the main pass: copy it, then draw the warp back into it
// (the load variant of the main pass). Leaves the image in PRESENT_SRC_KHR (the pass's final layout).
void SatelliteSim::recordTurbWarp(VkCommandBuffer cmd, VulkanContext &ctx, VkImage image)
{
    FPROF_SCOPE("turbWarp.record");
    int idx = -1;
    for (size_t i = 0; i < ctx.swapImages.size(); ++i)
        if (ctx.swapImages[i] == image)
            idx = (int)i;
    if (idx < 0 || idx >= (int)ctx.framebuffers.size())
        return;
    ctx.imageBarrier(cmd, turbFrameImg, VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                     VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkImageCopy rg{};
    rg.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    rg.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    rg.extent = {ctx.swapExtent.width, ctx.swapExtent.height, 1};
    vkCmdCopyImage(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, turbFrameImg, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &rg);
    ctx.imageBarrier(cmd, turbFrameImg, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                     VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    // The load pass expects TRANSFER_DST_OPTIMAL (it overwrites every pixel anyway).
    ctx.imageBarrier(cmd, image, VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                     VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                     VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkClearValue clears[2]{};
    clears[1].depthStencil = {1.0f, 0};
    VkRenderPassBeginInfo rbi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rbi.renderPass = ctx.renderPassLoad;
    rbi.framebuffer = ctx.framebuffers[idx];
    rbi.renderArea = {{0, 0}, ctx.swapExtent};
    rbi.clearValueCount = 2;
    rbi.pClearValues = clears;
    vkCmdBeginRenderPass(cmd, &rbi, VK_SUBPASS_CONTENTS_INLINE);
    VkViewport vp{0, 0, (float)ctx.swapExtent.width, (float)ctx.swapExtent.height, 0, 1};
    VkRect2D sc{{0, 0}, ctx.swapExtent};
    vkCmdSetViewport(cmd, 0, 1, &vp);
    vkCmdSetScissor(cmd, 0, 1, &sc);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, turbPipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, turbPipeLayout, 0, 1, &turbSet, 0, nullptr);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRenderPass(cmd);
    ++turbFrames_;
}

nlohmann::json SatelliteSim::turbWarpJson() const
{
    nlohmann::json j;
    j["strength"] = refrTurbulence;
    j["cell_size"] = turbCellSize;
    j["speed"] = turbSpeed;
    j["finest_cell_px"] = turbOctLoPx_;
    j["active"] = turbWarpActive_;
    j["max_rms_px"] = turbMaxPx_ / 3.0f;
    j["max_px_3sigma"] = turbMaxPx_;
    j["alt_fade"] = turbAltFade_;
    j["frames_run"] = turbFrames_;
    j["clock_s"] = turbClock_;
    j["twinkle_clock_s"] = twinkleClock_;
    j["pixel_arcsec"] = camera.fovYDeg * 3600.0 / std::max(1u, turbFrameExtent.height);
    return j;
}
