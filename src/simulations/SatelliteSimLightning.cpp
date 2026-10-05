// Lightning channels, their glow and their thunder (2026-10-05).
//
// cloud_v2_lightning.comp decides WHICH flashes happen (towers and storm cells, a deterministic schedule in sim
// time) and writes the list; this file reads the previous frame's list, builds each new flash's CHANNEL as
// geometry once (a fractal tree from its seed: the slanted main channel of a ground stroke with its branches,
// sub-branches and many dim tendrils, a spider network for a flash in or under the cloud, the tendrils of a red
// sprite), caches it while the flash lives, and each frame uploads the flashes' frames and the segments.
// lightning.vert/.frag draw them after the sky TAA at full resolution (channels as antialiased lines whose
// energy is kept when they are thinner than a pixel; the cloud lit from inside around each flash), blended
// so they saturate instead of blowing out. The same geometry schedules the THUNDER: the sound of each segment
// arrives at its distance / 343 m/s, so the roll's peals are the channel's own shape.
// It replaced cloud_march.comp's lightningCS (a 14-vertex channel and a glow, per pixel in the half-res
// composite, smeared by the sky TAA).
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

namespace
{
struct BoltRng
{
    uint64_t s;
    explicit BoltRng(uint64_t seed) : s(seed * 0x9E3779B97F4A7C15ull + 0x632BE59BD9B4E019ull) { next(); }
    uint32_t next()
    {
        s = s * 6364136223846793005ull + 1442695040888963407ull;
        uint32_t x = (uint32_t)(((s >> 18u) ^ s) >> 27u), r = (uint32_t)(s >> 59u);
        return (x >> r) | (x << ((32u - r) & 31u));
    }
    float u() { return (next() >> 8) * (1.0f / 16777216.0f); }
    float s1() { return 2.0f * u() - 1.0f; }
    float g() { return (u() + u() + u() - 1.5f) * 1.41f; }   // ~unit-variance bell
};

glm::vec3 perp(BoltRng &r, glm::vec3 d)
{
    glm::vec3 v(r.s1(), r.s1(), r.s1());
    v -= d * glm::dot(v, d);
    float l = glm::length(v);
    return l > 1e-4f ? v / l : glm::normalize(glm::cross(d, glm::vec3(0.0f, 0.0f, 1.0f)) + glm::vec3(1e-3f));
}

// A tortuous path from a to b: midpoint displacement perpendicular to each piece (rough x its length, self-
// similar), down to pieces of minLen. Appends the vertices after a.
void fractal(BoltRng &r, glm::vec3 a, glm::vec3 b, float rough, float minLen, std::vector<glm::vec3> &out, int depth = 0)
{
    const float L = glm::length(b - a);
    if (L <= minLen || depth > 12)
    {
        out.push_back(b);
        return;
    }
    const glm::vec3 d = (b - a) / L;
    const glm::vec3 m = 0.5f * (a + b) + perp(r, d) * (L * rough * r.g());
    fractal(r, a, m, rough, minLen, out, depth + 1);
    fractal(r, m, b, rough, minLen, out, depth + 1);
}
} // namespace

// Pack: bits 0-6 the flash's slot, 7-8 the order (0 main .. 3 tendril), 9-20 the arc (fraction of the tree's
// longest arc, 12 bits), 21-30 f (where along its own branch, 10 bits; a sprite's tendril: top 0 .. tip 1).
static uint32_t boltPack(uint32_t order, float arcFrac, float f)
{
    const uint32_t a = (uint32_t)std::clamp(arcFrac * 4095.0f + 0.5f, 0.0f, 4095.0f);
    const uint32_t ff = (uint32_t)std::clamp(f * 1023.0f + 0.5f, 0.0f, 1023.0f);
    return (order & 3u) << 7 | a << 9 | ff << 21;
}

void SatelliteSim::buildBoltTree(BoltTree &t, uint32_t seed, float distM)
{
    BoltRng r(((uint64_t)seed << 32) ^ t.id);
    t.segs.clear();
    t.arms.clear();
    const float pixAng = 2.0f * std::tan(glm::radians(camera.fovYDeg) * 0.5f) / 900.0f;
    // Detail: pieces down to ~3 pixels at the flash's distance ("Lightning tendrils" scales the counts).
    const float minLen = std::clamp(distM * pixAng * 3.0f, 4.0f, 2500.0f);
    const float dens = std::clamp(cv2LightningTendrils, 0.0f, 4.0f);
    const float far = std::clamp(distM / 60000.0f, 0.0f, 1.0f);   // far away: fewer tiny tendrils (sub-pixel anyway)
    struct V { glm::vec3 p; float arc; };
    float maxArc = 1.0f;
    // A polyline into segments; returns the vertices with their arc (from the origin along the tree).
    auto emit = [&](const std::vector<glm::vec3> &pts, float arc0, uint32_t order, float w0, float w1, std::vector<V> *keep) {
        float arc = arc0;
        const size_t n = pts.size();
        for (size_t i = 1; i < n; ++i)
        {
            const float f0 = (float)(i - 1) / (float)(n - 1), f1 = (float)i / (float)(n - 1);
            BoltSegC s{};
            s.p0 = pts[i - 1];
            s.p1 = pts[i];
            s.w = glm::mix(w0, w1, 0.5f * (f0 + f1));
            s.packed = order;   // arc and f filled once maxArc is known
            s.arc = arc;
            s.f = 0.5f * (f0 + f1);
            arc += glm::length(pts[i] - pts[i - 1]);
            maxArc = std::max(maxArc, arc);
            t.segs.push_back(s);
            if (keep)
                keep->push_back({pts[i], arc});
        }
    };
    auto path = [&](glm::vec3 a, glm::vec3 b, float rough, float ml) {
        std::vector<glm::vec3> p{a};
        fractal(r, a, b, rough, ml, p);
        return p;
    };
    // Side branches off a list of vertices: per metre of parent, `perM` branches of length lenFn(vertex), heading
    // dirFn(parent direction), as order `order`; recursion to the next order through `next`.
    std::function<void(const std::vector<V> &, uint32_t, float, float, float)> branches =
        [&](const std::vector<V> &par, uint32_t order, float perM, float wgt, float lenScale) {
            if (order > 3 || par.size() < 2 || t.segs.size() > 9000)
                return;
            for (size_t i = 1; i + 1 < par.size(); ++i)
            {
                const glm::vec3 pd = par[i + 1].p - par[i].p;
                const float pl = glm::length(pd);
                if (pl < 1e-3f || r.u() > perM * pl)
                    continue;
                const glm::vec3 d = pd / pl;
                glm::vec3 dir;
                float len;
                if (t.kind == 1)
                {
                    // Down and out from a ground stroke's channel: never upward, shorter the lower it starts.
                    const float h = std::max(par[i].p.z - t.groundLocal.z, 50.0f);
                    const glm::vec3 out = perp(r, d);
                    dir = glm::normalize(d * (0.3f + 0.4f * r.u()) + out * (0.5f + 0.7f * r.u()) + glm::vec3(0.0f, 0.0f, -0.6f * r.u()));
                    if (dir.z > -0.15f) dir = glm::normalize(dir + glm::vec3(0.0f, 0.0f, -0.4f));
                    len = std::min(h * (0.12f + 0.45f * r.u()), lenScale * (0.4f + r.u()));
                }
                else if (t.kind == 2)
                {
                    const glm::vec3 out = perp(r, d);
                    dir = glm::normalize(d * 0.8f + out * 0.45f);
                    len = lenScale * (0.3f + 0.5f * r.u());
                }
                else
                {
                    // A spider: sideways, nearly level, drooping a little at the finest orders.
                    glm::vec3 out = perp(r, d);
                    out.z *= 0.25f;
                    dir = glm::normalize(d * 0.35f + glm::normalize(out + glm::vec3(1e-4f)) + glm::vec3(0.0f, 0.0f, order >= 3 ? -0.35f : 0.0f));
                    len = lenScale * (0.4f + r.u());
                }
                if (len < minLen * 0.7f)
                    continue;
                const float rough = (t.kind == 2) ? 0.10f : (order >= 3 ? 0.3f : 0.24f);
                std::vector<glm::vec3> p = path(par[i].p, par[i].p + dir * len, rough, std::max(minLen * (order >= 3 ? 0.6f : 1.0f), len / 24.0f));
                std::vector<V> kv;
                emit(p, par[i].arc, order, wgt, wgt * 0.35f, &kv);
                const float childScale = len * (t.kind == 1 ? 0.45f : 0.5f);
                if (order + 1 <= 3)
                    branches(kv, order + 1, perM * (order + 1 == 3 ? 3.0f : 1.6f) * (order + 1 == 3 ? (1.0f - 0.8f * far) : 1.0f),
                             wgt * (order + 1 == 3 ? 0.5f : 0.5f), order + 1 == 3 ? std::min(childScale, 260.0f) : childScale);
            }
        };

    if (t.kind == 1)
    {
        // ── Cloud-to-ground: from the charge region in the cloud, a wander inside it, out of the cloud's flank
        // or base, and down SLANTED to the ground point (cloud_v2_lightning.comp put it 1.5-9 km aside). ──
        const glm::vec3 G = t.groundLocal;
        // The cloud base, never below the strike's ground (high terrain under a low base).
        const float zb = std::min(std::max(t.baseLocalZ, G.z + 400.0f), -100.0f);
        const glm::vec2 h(G.x, G.y);
        const glm::vec2 hd = glm::length(h) > 1.0f ? glm::normalize(h) : glm::vec2(1.0f, 0.0f);
        const glm::vec2 side(-hd.y, hd.x);
        // The in-cloud start wanders a little; the channel leaves the cloud near the origin's side of the ground
        // point and covers most of the sideways distance on its way down: a slanted stroke, not a plumb line.
        const glm::vec3 P1(hd * (0.1f * glm::length(h) * r.u()) + side * (800.0f * r.s1()), zb * (0.35f + 0.3f * r.u()));
        const float xf = 0.05f + 0.3f * r.u();
        const glm::vec3 X(h * xf + side * (600.0f * r.s1()), zb);
        std::vector<glm::vec3> main = path(glm::vec3(0.0f), P1, 0.2f, minLen);
        std::vector<glm::vec3> m2 = path(P1, X, 0.22f, minLen);
        main.insert(main.end(), m2.begin() + 1, m2.end());
        m2 = path(X, G, 0.2f, minLen);
        main.insert(main.end(), m2.begin() + 1, m2.end());
        std::vector<V> kv;
        emit(main, 0.0f, 0, 1.0f, 1.0f, &kv);
        t.mainArc = maxArc;
        t.exitLocal = X;
        // Branches: the stepped leader's, mostly from the channel's lower two thirds; then twigs and tendrils.
        branches(kv, 1, dens * 1.0f / 260.0f, 0.32f, 3000.0f);
        // Upward streamers from the ground near the strike (the ones the leader did not meet).
        const int nUp = 2 + (int)(r.u() * 4.0f);
        for (int k = 0; k < nUp; ++k)
        {
            const glm::vec3 g0 = G + glm::vec3(r.s1() * 120.0f, r.s1() * 120.0f, 0.0f);
            std::vector<glm::vec3> p = path(g0, g0 + glm::vec3(r.s1() * 30.0f, r.s1() * 30.0f, 40.0f + 200.0f * r.u()), 0.3f, std::max(minLen * 0.5f, 6.0f));
            emit(p, t.mainArc, 3, 0.06f, 0.02f, nullptr);
        }
    }
    else if (t.kind == 2)
    {
        // ── A red sprite, ~75 km up: a head, 20-45 tendrils hanging 12-35 km, forking as they fall and splaying
        // out (a jellyfish of roots), and a few streamers rising from the head. f = top 0 .. tip 1. ──
        const int nT = (int)((20.0f + 25.0f * r.u()) * std::max(dens, 0.3f));
        for (int k = 0; k < nT; ++k)
        {
            const float a = 6.2831853f * r.u(), rr = 6000.0f * std::sqrt(r.u());
            const glm::vec3 s0(std::cos(a) * rr, std::sin(a) * rr, 3000.0f * r.s1());
            const float ln = 12000.0f + 23000.0f * r.u();
            const glm::vec3 e = s0 + glm::vec3(std::cos(a) * rr * 0.6f + 2500.0f * r.s1(), std::sin(a) * rr * 0.6f + 2500.0f * r.s1(), -ln);
            std::vector<glm::vec3> p = path(s0, e, 0.07f, std::max(minLen, ln / 16.0f));
            std::vector<V> kv;
            emit(p, 0.0f, 0, 0.7f, 0.25f, &kv);
            branches(kv, 1, dens * 1.0f / 5000.0f, 0.4f, ln * 0.45f);
        }
        const int nU = 4 + (int)(r.u() * 6.0f);
        for (int k = 0; k < nU; ++k)
        {
            const glm::vec3 s0(3000.0f * r.s1(), 3000.0f * r.s1(), 4000.0f);
            std::vector<glm::vec3> p = path(s0, s0 + glm::vec3(2500.0f * r.s1(), 2500.0f * r.s1(), 5000.0f + 9000.0f * r.u()), 0.08f,
                                            std::max(minLen, 1500.0f));
            emit(p, 0.0f, 2, 0.25f, 0.05f, nullptr);
        }
        t.mainArc = maxArc;
    }
    else
    {
        // ── In the cloud (kind 0) or just under its base (kind 3, spider lightning): 3-6 arms spreading out
        // nearly level for 6-30 km, branching into a web of twigs and drooping tendrils, lit as it spreads. ──
        const int nA = 3 + (int)(r.u() * 4.0f);
        const float reach = 6000.0f + 24000.0f * r.u() * (0.5f + 0.5f * t.strength);
        const float a0 = 6.2831853f * r.u();
        for (int k = 0; k < nA; ++k)
        {
            const float a = a0 + 6.2831853f * ((float)k + 0.35f * r.s1()) / (float)nA;
            const float ln = reach * (0.45f + 0.55f * r.u());
            const glm::vec3 e(std::cos(a) * ln, std::sin(a) * ln, 350.0f * r.s1() + (t.kind == 3 ? 150.0f * r.u() : 0.0f));
            std::vector<glm::vec3> p = path(glm::vec3(0.0f), e, 0.2f, std::max(minLen, 60.0f));
            std::vector<V> kv;
            emit(p, 0.0f, 0, 0.5f, 0.25f, &kv);   // fainter than a return stroke
            std::vector<glm::vec4> arm;
            for (const V &v : kv)
                arm.push_back(glm::vec4(v.p, v.arc));
            t.arms.push_back(std::move(arm));
            branches(kv, 1, dens * 1.0f / 1500.0f, 0.4f, ln * 0.3f);
        }
        t.mainArc = maxArc;
    }
    t.maxArc = maxArc;
    for (BoltSegC &s : t.segs)
        s.packed = boltPack(s.packed, s.arc / maxArc, s.f);
}

void SatelliteSim::createLightningResources(VulkanContext &ctx)
{
    ctx.createBuffer(sizeof(GpuBoltFlash) * kBoltMaxFlashes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, boltFlashBuf, boltFlashMem);
    vkMapMemory(ctx.device, boltFlashMem, 0, VK_WHOLE_SIZE, 0, &boltFlashMapped);
    ctx.createBuffer(sizeof(GpuBoltSeg) * kBoltMaxSegs, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, boltSegBuf, boltSegMem);
    vkMapMemory(ctx.device, boltSegMem, 0, VK_WHOLE_SIZE, 0, &boltSegMapped);

    const VkShaderStageFlags st = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutBinding b[4] = {
        {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, st, nullptr},          // the flashes (frames, state, glow emitters)
        {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, st, nullptr},          // the channel segments
        {2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, st, nullptr},  // the cloud composite A (distance, km)
        {3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, st, nullptr},  // the cloud composite B (transmittance)
    };
    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    li.bindingCount = 4;
    li.pBindings = b;
    if (vkCreateDescriptorSetLayout(ctx.device, &li, nullptr, &boltDescLayout) != VK_SUCCESS)
        throw std::runtime_error("SatelliteSim: lightning descriptor layout");
    VkDescriptorPoolSize ps[2] = {{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2}, {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2}};
    VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pi.maxSets = 1;
    pi.poolSizeCount = 2;
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
    if (!boltDescSet || !cloudMarchTargetAView || !cloudMarchTargetBView)
        return;
    VkDescriptorBufferInfo fl{boltFlashBuf, 0, VK_WHOLE_SIZE};
    VkDescriptorBufferInfo sg{boltSegBuf, 0, VK_WHOLE_SIZE};
    VkDescriptorImageInfo ta{cloudMarchSampler, cloudMarchTargetAView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkDescriptorImageInfo tb{cloudMarchSampler, cloudMarchTargetBView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet w[4] = {};
    for (int i = 0; i < 4; ++i)
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
    vkUpdateDescriptorSets(ctx.device, 4, w, 0, nullptr);
}

void SatelliteSim::destroyLightningResources(VkDevice device)
{
    if (boltPipeline) vkDestroyPipeline(device, boltPipeline, nullptr);
    if (boltPipeLayout) vkDestroyPipelineLayout(device, boltPipeLayout, nullptr);
    if (boltDescPool) vkDestroyDescriptorPool(device, boltDescPool, nullptr);
    if (boltDescLayout) vkDestroyDescriptorSetLayout(device, boltDescLayout, nullptr);
    if (boltFlashMapped) vkUnmapMemory(device, boltFlashMem);
    if (boltSegMapped) vkUnmapMemory(device, boltSegMem);
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
    boltFlashCount = 0;
    boltSegCount = 0;
    ++boltFrame;
    // Only a list the lightning pass wrote LAST frame: otherwise it is stale (clouds off, from far orbit) and its
    // flashes would hang on screen.
    const bool fresh = cv2LightningObsDirValid;
    cv2LightningObsDirValid = false;
    if (!cv2FlashMapped || !boltFlashMapped || !boltSegMapped || (!fresh && boltInjected_.empty()))
        return;
    const glm::dvec3 U0 = glm::normalize(cv2LightningObsDir);
    const glm::dvec3 E0 = glm::normalize(glm::cross(glm::dvec3(0.0, 0.0, 1.0), U0));
    const glm::dvec3 N0 = glm::cross(U0, E0);
    auto toEcef = [&](const float *v) { return E0 * (double)v[0] + N0 * (double)v[1] + U0 * (double)v[2]; };
    // This frame's eye and ENU (as the rain's).
    const float ground = (terrainFrameMapped && (debugDisableMask & 1024u) == 0) ? terrainFrameMapped[1] : obsTerrainH;
    const glm::dvec3 up = glm::normalize(glm::dvec3(obsDir));
    const glm::dvec3 eye = up * (6371000.0 + std::max((double)ground + 2.0, (double)obsHeightOffset));
    const glm::dvec3 E = glm::normalize(glm::cross(glm::dvec3(0.0, 0.0, 1.0), up));
    const glm::dvec3 N = glm::cross(up, E);
    auto toEnu = [&](const glm::dvec3 &v) { return glm::vec3((float)glm::dot(v, E), (float)glm::dot(v, N), (float)glm::dot(v, up)); };
    const double now = (double)simDayJ2000 * 86400.0 + simSecInDay;

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
            BoltTree t;
            t.id = id;
            t.kind = kind;
            t.originEcef = toEcef(e);
            t.up = glm::normalize(t.originEcef);
            t.t1 = glm::normalize(glm::cross(glm::dvec3(0.0, 0.0, 1.0), t.up));
            t.t2 = glm::cross(t.up, t.t1);
            const double oR = glm::length(t.originEcef);
            t.baseLocalZ = (float)(6371000.0 + (double)e[12] - oR);
            t.strength = e[14];
            const glm::dvec3 g = toEcef(e + 4) - t.originEcef;
            t.groundLocal = glm::vec3((float)glm::dot(g, t.t1), (float)glm::dot(g, t.t2), (float)glm::dot(g, t.up));
            buildBoltTree(t, seed, (float)glm::length(t.originEcef - eye));
            if (kind != 2)
                queueThunder(t, eye, now - (double)age);
            it = boltTrees_.emplace(id, std::move(t)).first;
        }
        BoltTree &t = it->second;
        t.lastFrame = boltFrame;
        if (boltSegCount + (uint32_t)t.segs.size() > kBoltMaxSegs)
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
            for (size_t k = 0; k < t.arms.size() && k < 3; ++k)
            {
                const std::vector<glm::vec4> &arm = t.arms[k];
                glm::vec3 front = glm::vec3(arm.empty() ? glm::vec4(0.0f) : arm[0]);
                for (const glm::vec4 &v : arm)
                    if (v.w <= reveal)
                        front = glm::vec3(v);
                em[1 + k] = glm::vec4(local(front), 0.7f);
            }
        for (int k = 0; k < 4; ++k)
            F.e[k] = em[k];
        for (const GpuBoltSeg &s : t.segs)
        {
            GpuBoltSeg &d = outS[boltSegCount++];
            d = s;
            d.packed = s.packed | slot;
        }
    };
    for (uint32_t i = 0; i < n; ++i)
        addFlash(f + i * 16);
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
        float rec[16] = {};
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
        injectedRecs.push_back(std::vector<float>(rec, rec + 16));
    }
    for (const std::vector<float> &rv : injectedRecs)
        addFlash(rv.data());

    // Forget trees whose flash has ended (ids recur only in another slot).
    for (auto it = boltTrees_.begin(); it != boltTrees_.end();)
        it = (boltFrame - it->second.lastFrame > 120) ? boltTrees_.erase(it) : std::next(it);
}

// Inside the main render pass, after the scene and before the rain (recordDraw).
void SatelliteSim::recordLightning(VkCommandBuffer cmd, VulkanContext &ctx)
{
    if (!boltPipeline || boltFlashCount == 0)
        return;
    BoltDrawPC pc{};
    pc.skyView = camera.viewMatrix();
    pc.fovYRad = glm::radians(camera.fovYDeg);
    pc.screenW = (float)ctx.swapExtent.width;
    pc.screenH = (float)ctx.swapExtent.height;
    pc.aspect = pc.screenW / pc.screenH;
    pc.params = glm::vec4(skyExposure(), cv2LightningBolt, cv2LightningGlow, 0.0f);
    pc.params2 = glm::vec4(cv2HighlightRolloff, (float)boltFlashCount, 0.0f, 0.0f);
    {
        const float ground = (terrainFrameMapped && (debugDisableMask & 1024u) == 0) ? terrainFrameMapped[1] : obsTerrainH;
        pc.spare = glm::vec4(std::max(ground + 2.0f, obsHeightOffset), 0.0f, 0.0f, 0.0f);
    }
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, boltPipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, boltPipeLayout, 0, 1, &boltDescSet, 0, nullptr);
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
// The sound of each segment reaches the listener at its distance / 343 m/s, louder for a longer segment, a
// nearer one (1/r) and one lying across the line of sight (a tortuous channel radiates most perpendicular to
// itself), quieter for a twig. Binned at 50 ms from the first arrival: that envelope IS the roll — a channel
// running away from the listener rumbles long and low, one across the sky claps. The synth shapes noise by it
// (AmbientSynth thunder, pushEvent), low-passed by the distance.
void SatelliteSim::queueThunder(const BoltTree &t, const glm::dvec3 &eye, double flashStartS)
{
    const glm::dvec3 rel0 = t.originEcef - eye;
    if (glm::length(rel0) > 35000.0)
        return;
    constexpr int kBins = 300;
    constexpr float kBinS = 0.05f;
    std::vector<float> env(kBins, 0.0f);
    double tMin = 1e30;
    struct A { double ta; float a; };
    std::vector<A> arr;
    arr.reserve(t.segs.size());
    for (const GpuBoltSeg &s : t.segs)
    {
        const uint32_t order = (s.packed >> 7) & 3u;
        if (order >= 3)
            continue;
        const glm::vec3 m = 0.5f * (s.p0 + s.p1);
        const glm::dvec3 w = rel0 + t.t1 * (double)m.x + t.t2 * (double)m.y + t.up * (double)m.z;
        const double r = std::max(glm::length(w), 30.0);
        const glm::vec3 d = s.p1 - s.p0;
        const float l = glm::length(d);
        if (l < 1e-3f)
            continue;
        const glm::dvec3 dE = glm::normalize(t.t1 * (double)d.x + t.t2 * (double)d.y + t.up * (double)d.z);
        const double c = glm::dot(dE, w / r);
        const float a = l * s.w * (order == 0 ? 1.0f : 0.5f) * (float)((0.25 + 0.75 * (1.0 - c * c)) / r);
        const double ta = r / 343.0;
        tMin = std::min(tMin, ta);
        arr.push_back({ta, a});
    }
    if (arr.empty())
        return;
    for (const A &x : arr)
    {
        const int b = (int)((x.ta - tMin) / kBinS);
        if (b < kBins)
            env[b] += x.a;
    }
    float peak = 0.0f;
    for (float v : env)
        peak = std::max(peak, v);
    if (peak <= 0.0f)
        return;
    int last = 0;
    for (int i = 0; i < kBins; ++i)
    {
        env[i] /= peak;
        if (env[i] > 0.02f)
            last = i;
    }
    env.resize(last + 1);
    ThunderEvent ev;
    ev.arriveS = flashStartS + 0.05 + tMin;
    ev.distKm = (float)(tMin * 343.0 / 1000.0);
    ev.energy = t.kind == 1 ? 1.0f : 0.5f;
    const glm::dvec3 upE = glm::normalize(eye);
    const glm::dvec3 eE = glm::normalize(glm::cross(glm::dvec3(0.0, 0.0, 1.0), upE));
    const glm::dvec3 nE = glm::cross(upE, eE);
    const float bearing = (float)std::atan2(glm::dot(rel0, eE), glm::dot(rel0, nE));
    ev.pan = std::sin(bearing - glm::radians(camera.azDeg));
    ev.env = std::move(env);
    ev.binS = kBinS;
    thunderPending_.push_back(std::move(ev));
}
