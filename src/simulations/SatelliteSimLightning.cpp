// Lightning channels, their glow and their thunder (2026-10-05).
//
// cloud_v2_lightning.comp decides WHICH flashes happen (towers and storm cells, a deterministic schedule in sim
// time) and writes the list; this file reads the previous frame's list, builds each new flash's CHANNEL as
// geometry once (LightningBolt.cpp, 2026-11-21: a tree that is a pure function of the flash's own record, at full
// detail: the slanted main channel of a ground stroke with the branches its stepped leader left, a forking sheet
// for a flash in or under the cloud, the tendrils of a red sprite), fits it to the drawn ground (bolt_ground.comp),
// caches it while the flash lives, and each frame uploads the flashes' frames and the pieces to draw at this
// frame's level of detail. lightning.vert/.frag draw them after the sky TAA at full resolution (channels as
// antialiased lines whose energy is kept when they are thinner than a pixel; the cloud lit from inside around
// each flash), blended so they saturate instead of blowing out. The same geometry schedules the THUNDER: the
// sound of each segment arrives at its distance / 343 m/s, so the roll's peals are the channel's own shape.
// It replaced cloud_march.comp's lightningCS (a 14-vertex channel and a glow, per pixel in the half-res
// composite, smeared by the sky TAA).
#include "../FrameProfile.h"
#include "SatelliteSim.h"
#include "../AmbientSynth.h"
#include "../AudioSystem.h"
#include "../VulkanContext.h"
#include "../Log.h"
#include "Ambience.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <stdexcept>

// Pack: bits 0-6 the flash's slot, 7-8 the order (0 main .. 3 tendril), 9-20 the arc (fraction of the tree's
// longest arc, 12 bits), 21-30 f (where along its own branch, 10 bits; a sprite's tendril: top 0 .. tip 1).
static uint32_t boltPack(uint32_t order, float arcFrac, float f)
{
    const uint32_t a = (uint32_t)std::clamp(arcFrac * 4095.0f + 0.5f, 0.0f, 4095.0f);
    const uint32_t ff = (uint32_t)std::clamp(f * 1023.0f + 0.5f, 0.0f, 1023.0f);
    return (order & 3u) << 7 | a << 9 | ff << 21;
}

// A flash's channel from its own record alone (LightningBolt.cpp): the seed and id, the kind, the cloud's base and
// top and the strike point in the flash's Earth-fixed frame. Until 2026-11-21 it was built for the distance and the
// field of view it was first seen at (pieces of 3 px THERE, by a midpoint displacement that drew from one random
// stream), so the same flash was another tree from another place, and one first seen from far kept its few long
// straight pieces when the eye came close (issue_20261121_163317).
void SatelliteSim::buildBoltTree(BoltTree &t, uint32_t seed)
{
    bolt::Params p;
    p.seed = seed;
    p.id = t.id;
    p.kind = t.kind;
    p.ground = t.groundLocal;
    p.baseZ = t.baseLocalZ;
    p.topZ = t.topLocalZ;
    p.strength = t.strength;
    p.tendrils = std::clamp(cv2LightningTendrils, 0.0f, 4.0f);
    bolt::build(p, t.tree);
    t.hashBuilt = t.tree.hash;
    t.maxArc = t.tree.maxArc;
    t.mainArc = t.tree.mainArc;
    t.exitLocal = t.tree.exitLocal;
    t.fit = (t.kind == 2) ? 2 : 0;   // (a sprite is 75 km up: no ground to fit)
    t.draw.clear();
    t.drawLevels.clear();
    // The thunder's sources (addThunderSource): the channel in pieces of ~30 m, the same from anywhere.
    static thread_local std::vector<bolt::Seg> tmp;
    bolt::segments(t.tree, 30.0f, tmp, 6000);
    t.segs.resize(tmp.size());
    for (size_t i = 0; i < tmp.size(); ++i)
    {
        BoltSegC &s = t.segs[i];
        s.p0 = tmp[i].p0;
        s.p1 = tmp[i].p1;
        s.w = tmp[i].w;
        s.arc = tmp[i].arc;
        s.f = tmp[i].f;
        s.packed = boltPack(tmp[i].order, tmp[i].arc / t.maxArc, tmp[i].f);
    }
    ++boltTreesBuilt_;
}

// This frame's pieces of a tree: each path at the level whose pieces are ~2.5 px at the path's own distance from
// the eye (a spider's arm overhead is fine while its far arms are coarse), rebuilt only when a level changes. The
// vertices drawn are always vertices of the full tree: the level of detail never changes the shape.
void SatelliteSim::boltDrawSegs(BoltTree &t, const glm::dvec3 &eye, float pixAng)
{
    const glm::dvec3 d = eye - t.originEcef;
    const glm::vec3 eL((float)glm::dot(d, t.t1), (float)glm::dot(d, t.t2), (float)glm::dot(d, t.up));
    const size_t nP = t.tree.paths.size();
    static thread_local std::vector<int8_t> lv;
    lv.resize(nP);
    for (size_t i = 0; i < nP; ++i)
    {
        const bolt::Path &p = t.tree.paths[i];
        const float dist = std::max(glm::length(eL - p.centre) - p.radius, 0.0f);
        lv[i] = (int8_t)bolt::pathLevel(p, std::clamp(dist * pixAng * 2.5f, 4.0f, 2500.0f));
    }
    if (t.drawLevels.size() == nP && nP > 0 && std::memcmp(lv.data(), t.drawLevels.data(), nP) == 0)
        return;
    t.drawLevels = lv;
    static thread_local std::vector<bolt::Seg> tmp;
    bolt::segmentsAt(t.tree, lv.data(), tmp, 12000);
    t.draw.resize(tmp.size());
    for (size_t i = 0; i < tmp.size(); ++i)
    {
        GpuBoltSeg &s = t.draw[i];
        s.p0 = tmp[i].p0;
        s.p1 = tmp[i].p1;
        s.w = tmp[i].w;
        s.packed = boltPack(tmp[i].order, tmp[i].arc / t.maxArc, tmp[i].f);
    }
    ++boltDrawRebuilds_;
}

void SatelliteSim::createLightningResources(VulkanContext &ctx)
{
    // Mirrored (frames in flight): rebuilt every frame, only the used part reaches the GPU (recordLightning marks it).
    boltFlashMapped = createMirrored(ctx, sizeof(GpuBoltFlash) * kBoltMaxFlashes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                     boltFlashBuf, boltFlashMem, false, "boltFlashes");
    boltSegMapped = createMirrored(ctx, sizeof(GpuBoltSeg) * kBoltMaxSegs, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, boltSegBuf,
                                   boltSegMem, false, "boltSegments");

    const VkShaderStageFlags st = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutBinding b[7] = {
        {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, st, nullptr},          // the flashes (frames, state, glow emitters)
        {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, st, nullptr},          // the channel segments
        {2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, st, nullptr},  // the cloud composite A (distance, km)
        {3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, st, nullptr},  // the cloud composite B (transmittance)
        {4, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, st, nullptr},  // the half-res scene depth (m; engine lights)
        {5, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, st, nullptr},          // the dust list + the engine lights in its tail
        {6, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, st, nullptr},           // the meshes' distance (lit by sat_mesh.frag)
    };
    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    li.bindingCount = 7;
    li.pBindings = b;
    if (vkCreateDescriptorSetLayout(ctx.device, &li, nullptr, &boltDescLayout) != VK_SUCCESS)
        throw std::runtime_error("SatelliteSim: lightning descriptor layout");
    VkDescriptorPoolSize ps[3] = {{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 3}, {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 3},
                                  {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1}};
    VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pi.maxSets = 1;
    pi.poolSizeCount = 3;
    pi.pPoolSizes = ps;
    if (vkCreateDescriptorPool(ctx.device, &pi, nullptr, &boltDescPool) != VK_SUCCESS)
        throw std::runtime_error("SatelliteSim: lightning descriptor pool");
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = boltDescPool;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &boltDescLayout;
    if (vkAllocateDescriptorSets(ctx.device, &ai, &boltDescSet) != VK_SUCCESS)
        throw std::runtime_error("SatelliteSim: lightning descriptor set");
    VkPushConstantRange pcr{st, 0, sizeof(BoltDrawPC)};
    VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pl.setLayoutCount = 1;
    pl.pSetLayouts = &boltDescLayout;
    pl.pushConstantRangeCount = 1;
    pl.pPushConstantRanges = &pcr;
    if (vkCreatePipelineLayout(ctx.device, &pl, nullptr, &boltPipeLayout) != VK_SUCCESS)
        throw std::runtime_error("SatelliteSim: lightning pipeline layout");
    createLightningPipeline(ctx);
    writeLightningDescriptors(ctx);
    createBoltGround(ctx);
}

void SatelliteSim::createLightningPipeline(VulkanContext &ctx)
{
    VkShaderModule vert = ctx.loadShader("shaders/lightning.vert.spv");
    VkShaderModule frag = ctx.loadShader("shaders/lightning.frag.spv");
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
    VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    ds.depthTestEnable = VK_TRUE;   // the terrain hides a channel (the glow quads sit at depth 0)
    ds.depthWriteEnable = VK_FALSE;
    ds.depthCompareOp = VK_COMPARE_OP_LESS;
    // SCREEN blend: out = src + dst (1 - src). With the sky's tonemap 1 - exp(-x) it is exactly the tonemap of
    // the summed radiance, T(a + b) = T(a) + T(b) (1 - T(a)): light added after the tonemap saturates instead of
    // clipping, however many strokes overlap.
    VkPipelineColorBlendAttachmentState cba{};
    cba.blendEnable = VK_TRUE;
    cba.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
    cba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
    cba.colorBlendOp = VK_BLEND_OP_ADD;
    cba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    cba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    cba.alphaBlendOp = VK_BLEND_OP_ADD;
    cba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
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
    ci.layout = boltPipeLayout;
    ci.renderPass = ctx.renderPass;
    ci.subpass = 0;
    if (vkCreateGraphicsPipelines(ctx.device, ctx.pipelineCache, 1, &ci, nullptr, &boltPipeline) != VK_SUCCESS)
        throw std::runtime_error("SatelliteSim: lightning pipeline");
    vkDestroyShaderModule(ctx.device, vert, nullptr);
    vkDestroyShaderModule(ctx.device, frag, nullptr);
}

void SatelliteSim::writeLightningDescriptors(VulkanContext &ctx)
{
    if (!boltDescSet || !cloudMarchTargetAView || !cloudMarchTargetBView || !sceneDepthView || !kDustBuf)
        return;
    VkDescriptorBufferInfo fl{boltFlashBuf, 0, VK_WHOLE_SIZE};
    VkDescriptorBufferInfo sg{boltSegBuf, 0, VK_WHOLE_SIZE};
    VkDescriptorImageInfo ta{cloudMarchSampler, cloudMarchTargetAView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkDescriptorImageInfo tb{cloudMarchSampler, cloudMarchTargetBView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkDescriptorImageInfo sd{sceneDepthSampler, sceneDepthView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkDescriptorBufferInfo du{kDustBuf, 0, VK_WHOLE_SIZE};
    VkWriteDescriptorSet w[6] = {};
    for (int i = 0; i < 6; ++i)
    {
        w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w[i].dstSet = boltDescSet;
        w[i].dstBinding = (uint32_t)i;
        w[i].descriptorCount = 1;
    }
    w[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    w[0].pBufferInfo = &fl;
    w[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    w[1].pBufferInfo = &sg;
    w[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    w[2].pImageInfo = &ta;
    w[3].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    w[3].pImageInfo = &tb;
    w[4].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    w[4].pImageInfo = &sd;
    w[5].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    w[5].pBufferInfo = &du;
    frameSafeUpdateDescriptorSets(ctx.device, 6, w, 0, nullptr);
    if (meshRendererInit)
        writeLightningMeshDescriptor(ctx);
}

// Binding 6: the meshes' distance target (2026-10-21): the engine lights' ground term leaves mesh pixels to
// sat_mesh.frag's local lights. Rewritten whenever the mesh scene targets are (writeMeshSceneDescriptors).
void SatelliteSim::writeLightningMeshDescriptor(VulkanContext &ctx)
{
    if (!boltDescSet || !meshRenderer.sceneDistView())
        return;
    VkDescriptorImageInfo md{VK_NULL_HANDLE, meshRenderer.sceneDistView(), VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.dstSet = boltDescSet;
    w.dstBinding = 6;
    w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    w.pImageInfo = &md;
    frameSafeUpdateDescriptorSets(ctx.device, 1, &w, 0, nullptr);
}

void SatelliteSim::destroyLightningResources(VkDevice device)
{
    destroyBoltGround(device);
    if (boltPipeline) vkDestroyPipeline(device, boltPipeline, nullptr);
    if (boltPipeLayout) vkDestroyPipelineLayout(device, boltPipeLayout, nullptr);
    if (boltDescPool) vkDestroyDescriptorPool(device, boltDescPool, nullptr);
    if (boltDescLayout) vkDestroyDescriptorSetLayout(device, boltDescLayout, nullptr);
    if (ctx_ && boltFlashMapped) ctx_->mirrorForget(boltFlashMapped);
    if (ctx_ && boltSegMapped) ctx_->mirrorForget(boltSegMapped);
    if (boltFlashBuf) vkDestroyBuffer(device, boltFlashBuf, nullptr);
    if (boltFlashMem) vkFreeMemory(device, boltFlashMem, nullptr);
    if (boltSegBuf) vkDestroyBuffer(device, boltSegBuf, nullptr);
    if (boltSegMem) vkFreeMemory(device, boltSegMem, nullptr);
    boltPipeline = VK_NULL_HANDLE;
    boltPipeLayout = VK_NULL_HANDLE;
    boltDescPool = VK_NULL_HANDLE;
    boltDescSet = VK_NULL_HANDLE;
    boltDescLayout = VK_NULL_HANDLE;
    boltFlashMapped = boltSegMapped = nullptr;
    boltFlashBuf = boltSegBuf = VK_NULL_HANDLE;
    boltFlashMem = boltSegMem = VK_NULL_HANDLE;
}

// recordCompute, before the lightning pass is recorded again: the previous frame's flash list (host-mapped) in the
// frame that pass ran in (cv2LightningObsDir). Builds the new flashes' trees, drops the ended ones, and fills the
// buffers this frame's recordLightning draws from, in THIS frame's eye-centred ENU.
void SatelliteSim::updateLightningBolts()
{
    FPROF_SCOPE("updateLightningBolts");
    boltFlashCount = 0;
    boltSegCount = 0;
    boltProbeCount = 0;
    ++boltFrame;
    boltGroundCollect();   // the ground under the trees asked two frames ago: fit them
    // Only a list the lightning pass wrote LAST frame: otherwise it is stale (clouds off, from far orbit) and its
    // flashes would hang on screen.
    // (Per frame slot: the slot's copy of the flash buffer's head holds the list of the frame that filled it.)
    const bool fresh = cv2LightningObsDirValidS[ctx_->frameSlot];
    cv2LightningObsDirValidS[ctx_->frameSlot] = false;
    if (!cv2FlashMapped || !boltFlashMapped || !boltSegMapped || (!fresh && boltInjected_.empty()))
        return;
    const glm::dvec3 U0 = glm::normalize(cv2LightningObsDirS[ctx_->frameSlot]);
    const glm::dvec3 E0 = glm::normalize(glm::cross(glm::dvec3(0.0, 0.0, 1.0), U0));
    const glm::dvec3 N0 = glm::cross(U0, E0);
    // This frame's eye and ENU (as the rain's).
    const float ground = (terrainFrameMapped && (debugDisableMask & 1024u) == 0) ? terrainFrameMapped[1] : obsTerrainH;
    const glm::dvec3 up = glm::normalize(glm::dvec3(obsDir));
    const glm::dvec3 eye = up * (6371000.0 + std::max((double)ground + 2.0, (double)obsHeightOffset));
    const glm::dvec3 E = glm::normalize(glm::cross(glm::dvec3(0.0, 0.0, 1.0), up));
    const glm::dvec3 N = glm::cross(up, E);
    auto toEnu = [&](const glm::dvec3 &v) { return glm::vec3((float)glm::dot(v, E), (float)glm::dot(v, N), (float)glm::dot(v, up)); };
    const double now = (double)simDayJ2000 * 86400.0 + simSecInDay;
    const float pixAng = 2.0f * std::tan(glm::radians(camera.fovYDeg) * 0.5f) / (float)std::max<uint32_t>(ctx_->swapExtent.height, 1u);

    const uint32_t *hdr = (const uint32_t *)cv2FlashMapped;
    const float *f = (const float *)((const char *)cv2FlashMapped + 16);
    const uint32_t n = fresh ? std::min(hdr[0], kCv2FlashMax) : 0u;
    GpuBoltFlash *outF = (GpuBoltFlash *)boltFlashMapped;
    GpuBoltSeg *outS = (GpuBoltSeg *)boltSegMapped;
    std::vector<std::vector<float>> injectedRecs;
    auto addFlash = [&](const float *e) {
        if (boltFlashCount >= kBoltMaxFlashes)
            return;
        uint32_t id, seed;
        std::memcpy(&id, &e[8], 4);
        std::memcpy(&seed, &e[9], 4);
        const int kind = (int)(e[7] + 0.5f);
        const float age = e[10];
        auto it = boltTrees_.find(id);
        if (it == boltTrees_.end())
        {
            // From the record's EARTH-FIXED fields (e, f: the origin's and the strike point's directions and heights,
            // the same bits from every viewpoint), not its observer-frame positions (a, b: 0.5 m float steps about
            // the Earth's centre that differ from place to place): the tree's inputs, and so the tree, are identical
            // wherever the flash is seen from.
            BoltTree t;
            t.id = id;
            t.kind = kind;
            t.up = glm::normalize(glm::dvec3(e[16], e[17], e[18]));
            t.originEcef = t.up * (6371000.0 + (double)e[19]);
            t.t1 = glm::normalize(glm::cross(glm::dvec3(0.0, 0.0, 1.0), t.up));
            t.t2 = glm::cross(t.up, t.t1);
            t.baseLocalZ = e[12] - e[19];
            t.topLocalZ = e[13] - e[19];
            t.strength = e[14];
            if (kind == 1)
            {
                const glm::dvec3 g = glm::normalize(glm::dvec3(e[20], e[21], e[22])) * (6371000.0 + (double)e[23]) - t.originEcef;
                t.groundLocal = glm::vec3((float)glm::dot(g, t.t1), (float)glm::dot(g, t.t2), (float)glm::dot(g, t.up));
            }
            buildBoltTree(t, seed);
            // (One sound source a flash: a tree built again for a flash already heard, after its list dropped out for a
            // while, must not sound twice.)
            bool sounded = kind == 2;
            for (const ThunderSource &ts : thunderSources_)
                sounded = sounded || ts.id == id;
            if (!sounded)
                addThunderSource(t, now - (double)age);
            it = boltTrees_.emplace(id, std::move(t)).first;
        }
        BoltTree &t = it->second;
        t.lastFrame = boltFrame;
        boltDrawSegs(t, eye, pixAng);
        if (boltSegCount + (uint32_t)t.draw.size() > kBoltMaxSegs)
            return;

        const uint32_t slot = boltFlashCount++;
        GpuBoltFlash &F = outF[slot];
        const glm::vec3 o = toEnu(t.originEcef - eye);
        const glm::vec3 u = toEnu(t.up), a1 = toEnu(t.t1), a2 = toEnu(t.t2);
        F.o = glm::vec4(o, e[3]);
        F.u = glm::vec4(u, age);
        F.t1 = glm::vec4(a1, (float)kind);
        F.t2 = glm::vec4(a2, t.maxArc);
        // State: x the revealed arc (m): the stepped leader working down (kLeaderS) for a ground stroke, a spider
        // spreading at ~150 km/s; y the branches' share (a ground stroke's branches light with the first return
        // stroke and fade within ~0.1 s, the main channel carries the later strokes); z 1 during the leader.
        constexpr float kLeader = 0.05f;   // == kCv2LeaderS (cloud_lightning.glsl)
        float reveal = 1e9f, brMul = 1.0f, leader = 0.0f;
        if (kind == 1)
        {
            if (age < kLeader)
            {
                reveal = t.mainArc * std::pow(std::max(age, 0.0f) / kLeader, 1.3f);
                leader = 1.0f;
            }
            brMul = age < kLeader ? 0.0f : 0.2f + 0.8f * std::exp(-(age - kLeader) / 0.12f);
        }
        else if (kind != 2)
            reveal = 150000.0f * std::max(age - kLeader + 0.03f, 0.0f);
        F.m = glm::vec4(reveal, brMul, leader, 0.0f);
        // Where the cloud glows: the origin, and where the channel leaves the cloud (a ground stroke) or the spreading
        // fronts of three arms (a spider: the light moves through the cloud with it).
        glm::vec4 em[4] = {glm::vec4(o, 1.0f), glm::vec4(0.0f), glm::vec4(0.0f), glm::vec4(0.0f)};
        auto local = [&](glm::vec3 L) { return o + a1 * L.x + a2 * L.y + u * L.z; };
        if (kind == 1)
            em[1] = glm::vec4(local(t.exitLocal), 0.6f);
        else if (kind == 2)
            em[0] = glm::vec4(local(glm::vec3(0.0f, 0.0f, 5000.0f)), 1.0f);
        else
            for (size_t k = 0; k + 1 < t.tree.armStart.size() && k < 3; ++k)
                em[1 + k] = glm::vec4(local(bolt::armFront(t.tree, k, reveal)), 0.7f);
        for (int k = 0; k < 4; ++k)
            F.e[k] = em[k];
        for (const GpuBoltSeg &s : t.draw)
        {
            GpuBoltSeg &d = outS[boltSegCount++];
            d = s;
            d.packed = s.packed | slot;
        }
    };
    for (uint32_t i = 0; i < n; ++i)
        addFlash(f + i * kCv2FlashFloats);
    // Flashes placed by the harness (`lightning spawn`): the same records, timed on the CPU with the shader's schedule.
    for (auto it = boltInjected_.begin(); it != boltInjected_.end();)
    {
        const InjectedFlash &q = *it;
        const float age = (float)(now - q.startS);
        if (age > q.dur + 0.4f || age < -1.0f)
        {
            it = boltInjected_.erase(it);
            continue;
        }
        ++it;
        if (age < 0.0f || boltFlashCount >= kBoltMaxFlashes)
            continue;
        // == cv2FlashIntensity (cloud_lightning.glsl), strokes from the seed.
        float I = 0.0f;
        {
            const float a = age - 0.05f;
            const bool cg = q.kind == 1;
            if (a >= 0.0f)
            {
                const int nS = cg ? 1 + (int)(q.seed % 4u) : 2 + (int)(q.seed % 5u);
                const float tau = cg ? 0.035f : 0.06f;
                for (int k = 0; k < nS; ++k)
                {
                    const float tk = (k == 0) ? 0.0f : q.dur * ((float)k + 0.3f * (float)((q.seed >> (5 * k)) & 31u) / 31.0f) / (float)nS;
                    if (a >= tk)
                        I += (cg ? 1.0f : 0.6f) * std::exp(-(a - tk) / tau);
                }
                I += 0.3f * std::clamp(a / 0.02f, 0.0f, 1.0f) * std::exp(-a / (0.4f * q.dur + 0.05f));
            }
            if (q.kind == 2)
                I = std::clamp(age / 0.012f, 0.0f, 1.0f) * std::exp(-age / 0.07f);
        }
        // A record as the GPU writes it, in this frame's own ENU (the frame toEcef reads with).
        float rec[kCv2FlashFloats] = {};
        const glm::dvec3 oR = q.origin, gR = q.ground;
        auto put = [&](float *d, const glm::dvec3 &v) {
            d[0] = (float)glm::dot(v, E0); d[1] = (float)glm::dot(v, N0); d[2] = (float)glm::dot(v, U0);
        };
        put(rec, oR);
        rec[3] = I;
        put(rec + 4, gR);
        rec[7] = (float)q.kind;
        std::memcpy(&rec[8], &q.id, 4);
        std::memcpy(&rec[9], &q.seed, 4);
        rec[10] = age;
        rec[11] = (float)glm::length(oR - eye);
        rec[12] = q.base; rec[13] = q.top; rec[14] = q.strength; rec[15] = q.dur;
        {
            // The Earth-fixed fields, as the pass writes them (a float direction + an altitude).
            const glm::vec3 od = glm::vec3(glm::normalize(oR)), gd = glm::vec3(glm::normalize(gR));
            rec[16] = od.x; rec[17] = od.y; rec[18] = od.z; rec[19] = (float)(glm::length(oR) - 6371000.0);
            rec[20] = gd.x; rec[21] = gd.y; rec[22] = gd.z; rec[23] = (float)(glm::length(gR) - 6371000.0);
        }
        injectedRecs.push_back(std::vector<float>(rec, rec + kCv2FlashFloats));
    }
    for (const std::vector<float> &rv : injectedRecs)
        addFlash(rv.data());

    // Forget trees whose flash has ended (ids recur only in another slot).
    for (auto it = boltTrees_.begin(); it != boltTrees_.end();)
        it = (boltFrame - it->second.lastFrame > 120) ? boltTrees_.erase(it) : std::next(it);
    // The drawn ground under the new trees (answered two frames on: boltGroundCollect).
    boltGroundAsk(up * 6371000.0, E, N, up);
}

// ── The drawn ground under the channels (bolt_ground.comp; the members' note in SatelliteSim.h) ──────────────────
void SatelliteSim::createBoltGround(VulkanContext &ctx)
{
    if (boltProbePipeline)
        return;
    boltProbeMapped = createMirrored(ctx, sizeof(glm::vec4) * kBoltProbeMax,
                                     VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT, boltProbeBuf, boltProbeMem,
                                     false, "boltGroundProbe");
    rbBoltProbe = ctx.readbackCreate(sizeof(glm::vec4) * kBoltProbeMax, &boltProbeRead, "boltGroundProbe");
    VkDescriptorSetLayoutBinding b[4] = {};
    b[0] = {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    b[1] = {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    b[2] = {2, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    b[3] = {3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    li.bindingCount = 4;
    li.pBindings = b;
    vkCreateDescriptorSetLayout(ctx.device, &li, nullptr, &boltProbeDescLayout);
    VkDescriptorPoolSize ps[3] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2}, {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1},
                                  {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1}};
    VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pi.poolSizeCount = 3;
    pi.pPoolSizes = ps;
    pi.maxSets = 1;
    vkCreateDescriptorPool(ctx.device, &pi, nullptr, &boltProbeDescPool);
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = boltProbeDescPool;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &boltProbeDescLayout;
    vkAllocateDescriptorSets(ctx.device, &ai, &boltProbeDescSet);
    VkPushConstantRange pr{VK_SHADER_STAGE_COMPUTE_BIT, 0, 32};
    VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &boltProbeDescLayout;
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges = &pr;
    vkCreatePipelineLayout(ctx.device, &pli, nullptr, &boltProbePipeLayout);
    VkShaderModule mod = ctx.loadShader("shaders/bolt_ground.comp.spv");
    VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    ci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, mod, "main", nullptr};
    ci.layout = boltProbePipeLayout;
    if (vkCreateComputePipelines(ctx.device, ctx.pipelineCache, 1, &ci, nullptr, &boltProbePipeline) != VK_SUCCESS)
        boltProbePipeline = VK_NULL_HANDLE;   // (no fit: the channels keep the flash list's ground point)
    vkDestroyShaderModule(ctx.device, mod, nullptr);
}

void SatelliteSim::destroyBoltGround(VkDevice device)
{
    if (boltProbePipeline) vkDestroyPipeline(device, boltProbePipeline, nullptr);
    if (boltProbePipeLayout) vkDestroyPipelineLayout(device, boltProbePipeLayout, nullptr);
    if (boltProbeDescPool) vkDestroyDescriptorPool(device, boltProbeDescPool, nullptr);
    if (boltProbeDescLayout) vkDestroyDescriptorSetLayout(device, boltProbeDescLayout, nullptr);
    if (ctx_ && boltProbeMapped) ctx_->mirrorForget(boltProbeMapped);
    if (ctx_ && rbBoltProbe >= 0) ctx_->readbackDestroy(rbBoltProbe);
    if (boltProbeBuf) vkDestroyBuffer(device, boltProbeBuf, nullptr);
    if (boltProbeMem) vkFreeMemory(device, boltProbeMem, nullptr);
    boltProbePipeline = VK_NULL_HANDLE;
    boltProbePipeLayout = VK_NULL_HANDLE;
    boltProbeDescPool = VK_NULL_HANDLE;
    boltProbeDescSet = VK_NULL_HANDLE;
    boltProbeDescLayout = VK_NULL_HANDLE;
    boltProbeMapped = boltProbeRead = nullptr;
    rbBoltProbe = -1;
    boltProbeBuf = VK_NULL_HANDLE;
    boltProbeMem = VK_NULL_HANDLE;
}

// The trees not yet fitted (and, under the harness's `lightning check`, the fitted ones once more): their vertices'
// places from the observer's sea-level point, in this frame's ENU (the terrain functions' q).
void SatelliteSim::boltGroundAsk(const glm::dvec3 &seaPoint, const glm::dvec3 &E, const glm::dvec3 &N, const glm::dvec3 &up)
{
    if (!boltProbeMapped || !boltProbePipeline || !ctx_)
        return;
    glm::vec4 *out = (glm::vec4 *)boltProbeMapped;
    std::vector<BoltProbeJob> &jobs = boltProbeJobsS[ctx_->frameSlot];
    for (auto &kv : boltTrees_)
    {
        BoltTree &t = kv.second;
        if (t.lastFrame != boltFrame)
            continue;   // (only a flash on the list now)
        const bool check = t.fit == 2 && boltCheckAll_ && t.kind != 2;
        if (t.fit != 0 && !check)
            continue;
        const uint32_t n = (uint32_t)t.tree.verts.size();
        if (n == 0 || boltProbeCount + n > kBoltProbeMax)
            continue;
        const glm::dvec3 o = t.originEcef - seaPoint;
        const glm::dvec3 oE(glm::dot(o, E), glm::dot(o, N), glm::dot(o, up));
        const glm::dvec3 a1(glm::dot(t.t1, E), glm::dot(t.t1, N), glm::dot(t.t1, up));
        const glm::dvec3 a2(glm::dot(t.t2, E), glm::dot(t.t2, N), glm::dot(t.t2, up));
        const glm::dvec3 a3(glm::dot(t.up, E), glm::dot(t.up, N), glm::dot(t.up, up));
        for (uint32_t i = 0; i < n; ++i)
        {
            const glm::vec3 &v = t.tree.verts[i].p;
            out[boltProbeCount + i] = glm::vec4(glm::vec3(oE + a1 * (double)v.x + a2 * (double)v.y + a3 * (double)v.z), 0.0f);
        }
        jobs.push_back({t.id, boltProbeCount, n, check});
        boltProbeCount += n;
        t.fit = check ? 3 : 1;
    }
    if (boltProbeCount > 0)
        ctx_->mirrorDirty(boltProbeMapped, 0, sizeof(glm::vec4) * boltProbeCount);
}

// The answers of the frame that last used this slot (two frames ago with two frames in flight).
void SatelliteSim::boltGroundCollect()
{
    if (!ctx_)
        return;
    const uint32_t slot = ctx_->frameSlot;
    std::vector<BoltProbeJob> jobs;
    jobs.swap(boltProbeJobsS[slot]);
    const uint32_t copied = boltProbeCopyS[slot];
    boltProbeCopyS[slot] = 0;
    const glm::vec4 *res = (const glm::vec4 *)boltProbeRead;
    for (const BoltProbeJob &j : jobs)
    {
        auto it = boltTrees_.find(j.id);
        if (it == boltTrees_.end())
            continue;
        BoltTree &t = it->second;
        if (!res || j.first + j.count > copied || j.count != (uint32_t)t.tree.verts.size())
        {
            t.fit = j.check ? 2 : 0;   // (the frame was not drawn: ask again)
            continue;
        }
        const glm::vec4 *r = res + j.first;
        // The trunk's last vertex (a ground stroke's strike point).
        int tip = -1;
        for (const bolt::Path &p : t.tree.paths)
            if ((p.flags & bolt::kTrunk) && p.count >= 2)
                tip = (int)(p.first + p.count - 1);
        if (!j.check)
        {
            static thread_local std::vector<float> terr;
            terr.resize(j.count);
            for (uint32_t i = 0; i < j.count; ++i)
            {
                const float c = std::isfinite(r[i].w) ? r[i].w : 1.0e4f;
                terr[i] = t.tree.verts[i].p.z - c;
            }
            const float tipZ = tip >= 0 ? t.tree.verts[(size_t)tip].p.z : 0.0f;
            t.fitMinClear = bolt::fitTerrain(t.tree, terr);
            t.fitTipMove = tip >= 0 ? t.tree.verts[(size_t)tip].p.z - tipZ : 0.0f;
            t.fit = 2;
            t.drawLevels.clear();   // (its pieces are rebuilt from the fitted vertices)
        }
        else
        {
            float mn = 1.0e30f;
            for (const bolt::Path &p : t.tree.paths)
                for (uint32_t k = 0; k < p.count; ++k)
                    mn = std::min(mn, r[p.first + k].w);
            t.checkMinClear = mn > 1.0e29f ? 0.0f : mn;
            t.checkTip = tip >= 0 ? r[tip].w : 0.0f;
            t.fit = 4;
        }
    }
}

void SatelliteSim::recordBoltGround(VkCommandBuffer cmd, VulkanContext &ctx)
{
    if (boltProbeCount == 0 || !boltProbePipeline || !boltProbeBuf)
        return;
    VkSampler elevS = earthElevSampler ? earthElevSampler : noiseSampler;
    VkImageView elevV = earthElevView ? earthElevView : noiseTexView;
    VkSampler specS = earthSpecSampler ? earthSpecSampler : noiseSampler;
    VkImageView specV = earthSpecView ? earthSpecView : noiseTexView;
    const std::array<uint64_t, 4> bound = {(uint64_t)elevV, (uint64_t)specV, (uint64_t)cloudParamsBuf, (uint64_t)boltProbeBuf};
    if (bound != boltProbeBound_)
    {
        VkDescriptorImageInfo elev{elevS, elevV, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkDescriptorImageInfo spec{specS, specV, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkDescriptorBufferInfo ubo{cloudParamsBuf, 0, sizeof(GpuCloudParams)};
        VkDescriptorBufferInfo pb{boltProbeBuf, 0, VK_WHOLE_SIZE};
        VkWriteDescriptorSet w[4] = {};
        w[0] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, boltProbeDescSet, 0, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &elev, nullptr, nullptr};
        w[1] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, boltProbeDescSet, 1, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &spec, nullptr, nullptr};
        w[2] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, boltProbeDescSet, 2, 0, 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, nullptr, &ubo, nullptr};
        w[3] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, boltProbeDescSet, 3, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &pb, nullptr};
        frameSafeUpdateDescriptorSets(ctx.device, 4, w, 0, nullptr);
        boltProbeBound_ = bound;
    }
    struct
    {
        glm::vec4 obs;
        glm::uvec4 n;
    } pc;
    pc.obs = glm::vec4(glm::normalize(obsDir), obsHeightOffset);
    pc.n = glm::uvec4(boltProbeCount, 0u, 0u, 0u);
    static_assert(sizeof(pc) == 32);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, boltProbePipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, boltProbePipeLayout, 0, 1, &boltProbeDescSet, 0, nullptr);
    vkCmdPushConstants(cmd, boltProbePipeLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
    vkCmdDispatch(cmd, (boltProbeCount + 63) / 64, 1, 1);
    // (recordFrameReadbacks' barrier covers the copy into the slot's readback.)
}

// Inside the main render pass, after the scene and before the rain (recordDraw).
void SatelliteSim::recordLightning(VkCommandBuffer cmd, VulkanContext &ctx)
{
    if (!boltPipeline || (boltFlashCount == 0 && engineLightCount == 0))
        return;
    ctx.mirrorDirty(boltFlashMapped, 0, sizeof(GpuBoltFlash) * boltFlashCount);
    ctx.mirrorDirty(boltSegMapped, 0, sizeof(GpuBoltSeg) * boltSegCount);
    BoltDrawPC pc{};
    pc.skyView = camera.viewMatrix();
    pc.fovYRad = glm::radians(camera.fovYDeg);
    pc.screenW = (float)ctx.swapExtent.width;
    pc.screenH = (float)ctx.swapExtent.height;
    pc.aspect = pc.screenW / pc.screenH;
    pc.params = glm::vec4(skyExposure(), cv2LightningBolt, cv2LightningGlow, 0.0f);
    pc.params2 = glm::vec4(cv2HighlightRolloff, (float)boltFlashCount, (float)engineLightCount, 0.0f);
    {
        const float ground = (terrainFrameMapped && (debugDisableMask & 1024u) == 0) ? terrainFrameMapped[1] : obsTerrainH;
        pc.spare = glm::vec4(std::max(ground + 2.0f, obsHeightOffset), 0.0f, 0.0f, 0.0f);
    }
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, boltPipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, boltPipeLayout, 0, 1, &boltDescSet, 0, nullptr);
    // The burning engines' and the pads' light on the clouds, fog, haze and ground (2026-10-20, mode 2: one
    // full-screen triangle looping over the engine lights; lightning.frag).
    if (engineLightCount > 0)
    {
        pc.params.w = 2.0f;
        vkCmdPushConstants(cmd, boltPipeLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), &pc);
        vkCmdDraw(cmd, 3, 1, 0, 0);
    }
    if (boltFlashCount == 0)
        return;
    // The glow first (one full-screen triangle looping over the flashes' emitters), then the channels.
    pc.params.w = 1.0f;
    vkCmdPushConstants(cmd, boltPipeLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), &pc);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    if (boltSegCount > 0 && cv2LightningBolt > 0.0f)
    {
        pc.params.w = 0.0f;
        vkCmdPushConstants(cmd, boltPipeLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), &pc);
        vkCmdDraw(cmd, 4, boltSegCount, 0, 0);
    }
}

// ── Thunder from the channel ──────────────────────────────────────────────────────────────────────────
// A flash's channel as a sound source (2026-10-10): every piece (main channel, branches, twigs) with its place,
// direction and length, in Earth-fixed axes about the flash's origin. Nothing about the listener is decided here:
// thunderStep (SatelliteSimAmbience.cpp) meets the expanding shell of each piece with the listener every frame.
// (Until then the whole roll was an envelope computed here, for where the listener stood at the flash: flying
// on — the player moves at many times the speed of sound — it arrived at the wrong time from the wrong side.)
void SatelliteSim::addThunderSource(const BoltTree &t, double flashStartS)
{
    ThunderSource s;
    s.id = t.id;
    s.t0 = flashStartS + 0.05;   // the return stroke, after the leader (kCv2LeaderS)
    s.origin = t.originEcef;
    s.energy = t.kind == 1 ? 1.0f : 0.6f;   // a ground stroke; in cloud
    s.pts.reserve(t.segs.size());
    s.dirh.reserve(t.segs.size());
    for (const GpuBoltSeg &g : t.segs)
    {
        const uint32_t order = (g.packed >> 7) & 3u;
        if (order >= 3)
            continue;
        const glm::vec3 d = g.p1 - g.p0;
        const float l = glm::length(d);
        if (l < 1e-3f)
            continue;
        const glm::vec3 m = 0.5f * (g.p0 + g.p1);
        const glm::vec3 mE = glm::vec3(t.t1 * (double)m.x + t.t2 * (double)m.y + t.up * (double)m.z);
        const glm::vec3 dE = glm::vec3(glm::normalize(t.t1 * (double)d.x + t.t2 * (double)d.y + t.up * (double)d.z));
        s.pts.push_back(glm::vec4(mE, l * g.w * (order == 0 ? 1.0f : 0.5f)));
        s.dirh.push_back(glm::vec4(dE, 0.5f * l));
        s.extentM = std::max(s.extentM, glm::length(mE) + 0.5f * l);
    }
    if (s.pts.empty())
        return;
    thunderInsertSource(std::move(s), t.kind);
}
