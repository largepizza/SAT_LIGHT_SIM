// SatelliteSimReflectorSites.cpp — the reflector-site census and the Missions window's Reflectors tab (2026-11-24).
//
// The main orbit dispatch (sat_orbit.comp) resolves a Reflect mirror's ground target only for the satellites above
// the observer's horizon, so the beams the CPU reads back are the local ones: finding a lit site meant searching the
// night side by hand. The census is sat_orbit.comp compiled a second time with -DSAT_CENSUS (CMakeLists.txt): the
// same target choice (findWinner at the mirror's lock-window start) for every live roster mirror, counting per site
// the sunlit mirrors aimed at it and summing their directions. It runs only while something asks for it (the tab is
// open, a harness query), right after the orbit dispatch with the same descriptor set and push constants, and is read
// back by the frames-in-flight readback two frames later. ~5000 mirrors x 201 targets: a few hundredths of a ms.
//
// The tab lists the sites busiest first. Each row's one Watch (the event convention: already happening, 1x, in
// frame) stands the observer on the ground 15 km from the site, side-on to its mean beam, looking at the site.

#include "../FrameProfile.h"
#include "SatelliteSim.h"
#include "../UIRenderer.h"
#include "../VulkanContext.h"
#include "clay.h"
#include "UIPalette.h"
#include "SatPhotometry.h"

#include <nlohmann/json.hpp>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace
{
constexpr double kPiR = 3.14159265358979323846;
double degR(double r) { return r * (180.0 / kPiR); }
double radR(double d) { return d * (kPiR / 180.0); }
Clay_String csR(const char *s) { return Clay_String{false, (int32_t)strlen(s), s}; }
const char *compass8(double azDeg)
{
    static const char *k[] = {"north", "northeast", "east", "southeast", "south", "southwest", "west", "northwest"};
    int i = (int)std::floor(std::fmod(azDeg + 360.0 + 22.5, 360.0) / 45.0);
    return k[std::clamp(i, 0, 7)];
}
// A site's local east / north / up (ECEF), from its unit vector.
void siteEnu(const glm::dvec3 &u, glm::dvec3 &e, glm::dvec3 &n)
{
    e = glm::cross(glm::dvec3(0, 0, 1), u);
    e = glm::length(e) > 1e-9 ? glm::normalize(e) : glm::dvec3(0, 1, 0);
    n = glm::cross(u, e);
}
glm::dvec3 eciToEcefR(const glm::dvec3 &v, double t)
{
    const double th = earthRotationAngle(t), c = std::cos(th), s = std::sin(th);
    return glm::dvec3(c * v.x + s * v.y, -s * v.x + c * v.y, v.z);
}
} // namespace

// ── GPU ─────────────────────────────────────────────────────────────────────────────────────────────────────────────
void SatelliteSim::createReflectorCensus(VulkanContext &ctx)
{
    ctx.createBuffer(sizeof(GpuReflectorCensus),
                     VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                     VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, censusBuf, censusMem);
    rbCensus = ctx.readbackCreate(sizeof(GpuReflectorCensus), &censusMapped, "reflectorCensus");

    VkDescriptorSetLayoutBinding b{0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    li.bindingCount = 1;
    li.pBindings = &b;
    vkCreateDescriptorSetLayout(ctx.device, &li, nullptr, &censusDescLayout);
    VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1};
    VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pi.poolSizeCount = 1;
    pi.pPoolSizes = &ps;
    pi.maxSets = 1;
    vkCreateDescriptorPool(ctx.device, &pi, nullptr, &censusDescPool);
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = censusDescPool;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &censusDescLayout;
    vkAllocateDescriptorSets(ctx.device, &ai, &censusDescSet);
    VkDescriptorBufferInfo info{censusBuf, 0, VK_WHOLE_SIZE};
    VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, censusDescSet, 0, 0, 1,
                           VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &info, nullptr};
    vkUpdateDescriptorSets(ctx.device, 1, &w, 0, nullptr);

    // Set 0 is the orbit dispatch's own (its layout: the census binds orbitDescSet as it stands), set 1 the census.
    const VkDescriptorSetLayout sets[2] = {orbitDescLayout, censusDescLayout};
    VkPushConstantRange pcr{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(SatOrbitPC)};
    VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pli.setLayoutCount = 2;
    pli.pSetLayouts = sets;
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges = &pcr;
    vkCreatePipelineLayout(ctx.device, &pli, nullptr, &censusPipeLayout);

    VkShaderModule mod = ctx.loadShader("shaders/sat_orbit_census.comp.spv");
    VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    ci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, mod, "main", nullptr};
    ci.layout = censusPipeLayout;
    if (vkCreateComputePipelines(ctx.device, ctx.pipelineCache, 1, &ci, nullptr, &censusPipeline) != VK_SUCCESS)
        censusPipeline = VK_NULL_HANDLE;
    vkDestroyShaderModule(ctx.device, mod, nullptr);
}

void SatelliteSim::destroyReflectorCensus(VkDevice device)
{
    if (censusPipeline) vkDestroyPipeline(device, censusPipeline, nullptr);
    if (censusPipeLayout) vkDestroyPipelineLayout(device, censusPipeLayout, nullptr);
    if (censusDescPool) vkDestroyDescriptorPool(device, censusDescPool, nullptr);
    if (censusDescLayout) vkDestroyDescriptorSetLayout(device, censusDescLayout, nullptr);
    if (censusBuf) vkDestroyBuffer(device, censusBuf, nullptr);
    if (censusMem) vkFreeMemory(device, censusMem, nullptr);
    if (ctx_ && rbCensus >= 0) ctx_->readbackDestroy(rbCensus);
    censusPipeline = VK_NULL_HANDLE;
    censusPipeLayout = VK_NULL_HANDLE;
    censusDescPool = VK_NULL_HANDLE;
    censusDescLayout = VK_NULL_HANDLE;
    censusBuf = VK_NULL_HANDLE;
    censusMem = VK_NULL_HANDLE;
    rbCensus = -1;
}

void SatelliteSim::reflectorCensusWant(int frames)
{
    const uint64_t f = (ctx_ ? ctx_->frameIndex : 0) + (uint64_t)std::max(frames, 1);
    censusWantFrame = std::max(censusWantFrame, f);
}

// The rows from the census the slot's previous frame copied back (call before this frame records its own).
void SatelliteSim::reflectorCensusCollect(VulkanContext &ctx)
{
    const uint32_t slot = ctx.frameSlot;
    if (!censusRanS[slot])
        return;
    censusRanS[slot] = false;
    const GpuReflectorCensus *c = (const GpuReflectorCensus *)censusMapped;
    if (!c)
        return;
    const double t = censusSimTS[slot];
    const glm::dvec3 sunE = glm::normalize(eciToEcefR(glm::normalize(sunDirEciAt(t)), t));
    rsMirrors = (int)c->mirrors;
    rsLit = (int)c->lit;
    rsRows.clear();
    for (int k = 0; k < reflectorTargetCount && k < kNumReflectorTargets; ++k)
    {
        if (c->site[k] == 0)
            continue;
        ReflectorSiteRow r;
        r.site = k;
        r.mirrors = (int)c->site[k];
        const glm::dvec3 u = glm::normalize(glm::dvec3(reflectorTargetsECEF[k]));
        glm::dvec3 e, n;
        siteEnu(u, e, n);
        const glm::dvec3 d = glm::dvec3(c->dir[3 * k], c->dir[3 * k + 1], c->dir[3 * k + 2]) / (1024.0 * r.mirrors);
        const double len = glm::length(d);
        if (len > 1e-6)
        {
            const glm::dvec3 m = d / len;
            r.fromAzDeg = (float)std::fmod(degR(std::atan2(glm::dot(m, e), glm::dot(m, n))) + 360.0, 360.0);
            r.fromElDeg = (float)degR(std::asin(std::clamp(glm::dot(m, u), -1.0, 1.0)));
        }
        r.spreadDeg = (float)degR(std::acos(std::clamp(len, 0.0, 1.0)));
        r.sunElDeg = (float)degR(std::asin(std::clamp(glm::dot(sunE, u), -1.0, 1.0)));
        rsRows.push_back(r);
    }
    std::sort(rsRows.begin(), rsRows.end(), [](const ReflectorSiteRow &a, const ReflectorSiteRow &b) {
        return a.mirrors != b.mirrors ? a.mirrors > b.mirrors : a.site < b.site;
    });
    rsSimT = t;
    ++rsGen;
}

void SatelliteSim::recordReflectorCensus(VkCommandBuffer cmd, VulkanContext &ctx, const SatOrbitPC &pc)
{
    reflectorCensusCollect(ctx);
    if (!censusPipeline || !censusBuf || ctx.frameIndex > censusWantFrame)
        return;
    // The roster range holding site-aiming types (the Reflect mirrors: one constellation, so one short range).
    const size_t nRoster = std::min<size_t>(satOrbits.size(), activeSatCount);
    if (censusRangeOf != nRoster)
    {
        censusRangeOf = nRoster;
        censusFirst = censusEnd = 0;
        bool any = false;
        for (size_t i = 0; i < nRoster; ++i)
        {
            const uint32_t ti = satOrbits[i].typeIdx;
            if (ti >= satTypes.size() || !attUsesGroundSite(satTypes[ti].groups))
                continue;
            if (!any)
                censusFirst = (uint32_t)i;
            censusEnd = (uint32_t)i + 1u;
            any = true;
        }
        rsHaveMirrors = any;
    }
    if (censusEnd <= censusFirst)
    {
        rsRows.clear();
        rsMirrors = rsLit = 0;
        rsSimT = (double)simDayJ2000 * 86400.0 + simSecInDay;
        ++rsGen;
        return;
    }
    const uint32_t hdr[4] = {censusFirst, censusEnd, 0u, 0u};
    vkCmdUpdateBuffer(cmd, censusBuf, 0, sizeof(hdr), hdr);
    vkCmdFillBuffer(cmd, censusBuf, sizeof(hdr), sizeof(GpuReflectorCensus) - sizeof(hdr), 0u);
    VkBufferMemoryBarrier bmb{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
    bmb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    bmb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    bmb.srcQueueFamilyIndex = bmb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    bmb.buffer = censusBuf;
    bmb.size = VK_WHOLE_SIZE;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 1,
                         &bmb, 0, nullptr);
    const VkDescriptorSet sets[2] = {orbitDescSet, censusDescSet};
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, censusPipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, censusPipeLayout, 0, 2, sets, 0, nullptr);
    vkCmdPushConstants(cmd, censusPipeLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(SatOrbitPC), &pc);
    vkCmdDispatch(cmd, (censusEnd - censusFirst + 63u) / 64u, 1, 1);
    censusRanS[ctx.frameSlot] = true; // recordFrameReadbacks copies it back
    censusSimTS[ctx.frameSlot] = (double)simDayJ2000 * 86400.0 + simSecInDay;
}

// ── The Watch ───────────────────────────────────────────────────────────────────────────────────────────────────────
bool SatelliteSim::reflectorSiteWatch(int site, std::string &msg)
{
    FPROF_SCOPE("reflectorSiteWatch");
    const ReflectorSiteRow *row = nullptr;
    for (const ReflectorSiteRow &r : rsRows)
        if (r.site == site)
            row = &r;
    if (!row || site < 0 || site >= reflectorTargetCount)
    {
        msg = tx(Txt::ValReflectorSiteNotLit);
        return false;
    }
    const ReflectorSiteRow R = *row; // (rsRows is rebuilt while we move)
    const glm::dvec3 u = glm::normalize(glm::dvec3(reflectorTargetsECEF[site]));
    glm::dvec3 e, n;
    siteEnu(u, e, n);
    // 15 km from the site, side-on to its mean beam (the beams cross the view instead of pointing at the eye).
    const double kDistM = 15000.0;
    const double brg = radR(R.fromAzDeg + 90.0);
    const glm::dvec3 off = (std::sin(brg) * e + std::cos(brg) * n) * (kDistM / 6371000.0);
    const glm::dvec3 obs = glm::normalize(u + off);
    timePaused = false;
    timeScaleIdx = 0;
    if (followActive)
        stopFollow();
    harnessTrack_ = 0;
    stopTrack();
    const double now = (double)simDayJ2000 * 86400.0 + simSecInDay;
    setObserverLatLon(degR(std::asin(std::clamp(obs.z, -1.0, 1.0))), degR(std::atan2(obs.y, obs.x)));
    obsHeightOffset = std::max(0.0f, cpuTerrainHeightM(obsLatDeg, obsLonDeg)) + 2.0f;
    updatePositions(now, 0.0f);
    // Look at the air 1.5 km over the site: the lit ground low in the frame, the beams coming down into it.
    glm::dvec3 oe, on;
    siteEnu(obs, oe, on);
    const double eyeR = 6371000.0 + std::max((double)obsHeightOffset, (double)obsTerrainH) + 2.0;
    const glm::dvec3 d = glm::normalize(u * ((double)reflectorTargetsRadiusM[site] + 1500.0) - obs * eyeR);
    aimCameraAzEl((float)degR(std::atan2(glm::dot(d, oe), glm::dot(d, on))),
                  (float)degR(std::asin(std::clamp(glm::dot(d, obs), -1.0, 1.0))));
    camera.fovYDeg = std::max(camera.fovYDeg, 60.0f);
    char b[300];
    snprintf(b, sizeof(b), tx(Txt::FmtWatchingReflectorSite), reflectorName[site].c_str(), R.mirrors, kDistM / 1000.0,
             compass8(R.fromAzDeg + 90.0));
    msg = b;
    return true;
}

nlohmann::json SatelliteSim::reflectorSitesJson()
{
    nlohmann::json j;
    j["have_mirrors"] = rsHaveMirrors;
    j["mirrors"] = rsMirrors;
    j["lit"] = rsLit;
    j["census_gen"] = rsGen;
    const double now = (double)simDayJ2000 * 86400.0 + simSecInDay;
    j["age_s"] = rsSimT > -1e299 ? now - rsSimT : -1.0;
    nlohmann::json rows = nlohmann::json::array();
    for (const ReflectorSiteRow &r : rsRows)
    {
        const ReflectorKind k = reflectorKind[r.site];
        rows.push_back({{"site", r.site}, {"name", reflectorName[r.site]},
                        {"kind", k == ReflectorKind::Solar ? "solar" : k == ReflectorKind::Agriculture ? "agriculture"
                                 : k == ReflectorKind::Daylight ? "daylight" : "none"},
                        {"lat", reflectorLatDeg[r.site]}, {"lon", reflectorLonDeg[r.site]}, {"mirrors", r.mirrors},
                        {"from_az_deg", r.fromAzDeg}, {"from_el_deg", r.fromElDeg}, {"spread_deg", r.spreadDeg},
                        {"sun_el_deg", r.sunElDeg}, {"converged_here", beamSiteConverged[r.site]}});
    }
    j["sites"] = rows;
    return j;
}

// ── The tab ─────────────────────────────────────────────────────────────────────────────────────────────────────────
void SatelliteSim::buildReflectorSitesTab(const UIInput &inp, UIRenderer &ui)
{
    reflectorCensusWant(30); // keep counting while the tab is drawn
    auto text = [&](const char *s, Clay_Color c, int size, bool wrap = false) {
        CLAY_TEXT(csR(s), CLAY_TEXT_CONFIG({.textColor = c, .fontSize = fs(size),
                                            .wrapMode = wrap ? CLAY_TEXT_WRAP_WORDS : CLAY_TEXT_WRAP_NONE}));
    };
    uiSection("MsRSec", 0, tx(Txt::SecSitesLitNow));
    if (!rsHaveMirrors || (rsSimT > -1e299 && rsMirrors == 0))
    {
        text(tx(Txt::NoteNoReflectorMirrors), Pal::textDim, 11, true);
        return;
    }
    if (rsSimT <= -1e299)
    {
        text(tx(Txt::NoteCountingMirrors), Pal::textDim, 11);
        return;
    }
    text(msStr(tx(Txt::FmtReflectorSummary), rsLit, rsMirrors, (int)rsRows.size()), Pal::textDim, 11, true);
    if (rsRows.empty())
    {
        text(tx(Txt::NoteNoReflectorSiteLit), Pal::textDim, 11, true);
        return;
    }
    for (size_t i = 0; i < rsRows.size(); ++i)
    {
        const ReflectorSiteRow &r = rsRows[i];
        const int id = (int)i;
        CLAY(CLAY_IDI("MsRRow", id), {.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0)},
                                                 .padding = {8, 8, 5, 5},
                                                 .childGap = 8,
                                                 .childAlignment = {.y = CLAY_ALIGN_Y_CENTER},
                                                 .layoutDirection = CLAY_LEFT_TO_RIGHT},
                                      .backgroundColor = Pal::cardBg,
                                      .cornerRadius = CLAY_CORNER_RADIUS(4),
                                      .border = {.color = Pal::cardBorder, .width = CLAY_BORDER_ALL(1)}})
        {
            CLAY(CLAY_IDI("MsRText", id), {.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0)},
                                                      .childGap = 3,
                                                      .layoutDirection = CLAY_TOP_TO_BOTTOM}})
            {
                CLAY(CLAY_IDI("MsRTitle", id), {.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0)},
                                                           .childGap = 6,
                                                           .childAlignment = {.y = CLAY_ALIGN_Y_CENTER},
                                                           .layoutDirection = CLAY_LEFT_TO_RIGHT}})
                {
                    const ReflectorKind k = reflectorKind[r.site];
                    const char *tagT = k == ReflectorKind::Solar ? tx(Txt::TagReflectorSolar)
                                     : k == ReflectorKind::Agriculture ? tx(Txt::TagReflectorFarmland)
                                     : k == ReflectorKind::Daylight ? tx(Txt::TagReflectorPolarTown) : nullptr;
                    if (tagT)
                        CLAY(CLAY_IDI("MsRTag", id), {.layout = {.sizing = {CLAY_SIZING_FIT(0), CLAY_SIZING_FIT(0)},
                                                                 .padding = {5, 5, 1, 1}},
                                                      .backgroundColor = k == ReflectorKind::Daylight ? Pal::tagTwilight
                                                                       : k == ReflectorKind::Solar ? Pal::tagCrew
                                                                                                   : Pal::progressFill,
                                                      .cornerRadius = CLAY_CORNER_RADIUS(3)})
                        {
                            text(tagT, Pal::textBright, 10);
                        }
                    text(msStr("%s", reflectorName[r.site].c_str()), Pal::textPrimary, 12, true);
                }
                text(msStr(tx(Txt::FmtReflectorRowStatus), r.mirrors, compass8(r.fromAzDeg), r.fromElDeg, r.sunElDeg),
                     Pal::textDim, 11, true);
            }
            if (uiButton(inp, ui, "MsRWatch", id, tx(Txt::BtnWatch), tx(Txt::TipWatchReflectorSite)))
                rsWatchPending = r.site; // applied after the layout (buildMissionsWindow): it moves the observer
        }
    }
}
