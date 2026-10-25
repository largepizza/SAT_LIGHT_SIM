// SatelliteSimKesslerTracker.cpp — the Missions window's "Kessler" tab (2026-10-21, issue 20261021_175638).
//
// The user: "This window can be a kessler tracker, which can contain a much more stylized view of collision counts, all
// the statistics. A small globe with deadbands and collision probability zones displayed? Markers for the last
// significant collisions?" It replaced the Missions window's Controls tab (those settings moved to Settings > Launches &
// debris). What it shows:
//   - THE SUN'S FURY strip: the CME's countdown, or DAY n of the cascade (and "computing" while the frontier holds time);
//   - big counts (uiStatTile): collisions, catastrophic, dead / destroyed / re-entered satellites, pieces, Kp;
//   - a GLOBE seen from the equator (a CPU raster, kTrN^2, uploaded in recordCompute) with the debris shells drawn round
//     it as a polar plot: radius = altitude (1.6x exaggerated), angle = latitude — the limb of an equatorial view IS a
//     meridian, so each latitude band's density sits where that latitude meets the sky. Hot colours = the chance a year
//     that a 10 m^2 satellite there is hit by a tracked object (density x 10 km/s x 10 m^2 x a year), blue = where the
//     working satellites fly. Markers for the latest significant breakups at their (exaggerated) place, numbered;
//     drag the globe to turn it. Your place is a cyan dot;
//   - the collisions per day since the CME (UIPlot);
//   - the latest breakups (kind, objects, when, where; Watch = the clock 8 s before it, 80 km beside it).
// Cost: everything comes from the counts walk (kesslerCountsWalk: spread over frames while kesslerStateCached is read),
// so nothing here walks the event log or the pool; the globe is re-rasterised only when its inputs change (a published
// walk, a drag, the Sun moving ~1 deg, at most 4 times a second) — ~1 ms on the main thread when it does.
#include "../FrameProfile.h"
#include "SatelliteSim.h"
#include "../UIRenderer.h"
#include "../VulkanContext.h"
#include "../Log.h"
#include "clay.h"
#include "UIPalette.h"

#include <nlohmann/json.hpp>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <chrono>
#include <ctime>
#include <functional>

namespace
{
constexpr double kR = 6371000.0;
constexpr double kTrExtent = 1.62;      // the image's half width, in Earth radii
constexpr double kTrAltK = 1.6 / 6371.0; // exaggeration: radius = 1 + kTrAltK x altitude (km)
constexpr double kTrAlt0Km = 150.0, kTrDAltKm = 50.0;
// The danger scale: the chance a year that a 10 m^2 satellite is hit, 1 - exp(-n v A t), n per km^3:
// v = 10 km/s, A = 1e-5 km^2, t = 3.156e7 s.
constexpr double kTrHitK = 10.0 * 1.0e-5 * 3.156e7;

double wallNow() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

glm::vec4 palV(const Clay_Color &c, float a = -1.0f)
{
    return glm::vec4(c.r / 255.0f, c.g / 255.0f, c.b / 255.0f, a >= 0.0f ? a : c.a / 255.0f);
}

float srgbToLin(uint8_t v)
{
    static float lut[256];
    static bool init = false;
    if (!init)
    {
        for (int i = 0; i < 256; ++i)
        {
            const double c = i / 255.0;
            lut[i] = (float)(c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4));
        }
        init = true;
    }
    return lut[v];
}
uint8_t linToSrgb(float v)
{
    static uint8_t lut[4096];
    static bool init = false;
    if (!init)
    {
        for (int i = 0; i < 4096; ++i)
        {
            const double c = i / 4095.0;
            const double s = c <= 0.0031308 ? 12.92 * c : 1.055 * std::pow(c, 1.0 / 2.4) - 0.055;
            lut[i] = (uint8_t)std::clamp((int)std::lround(s * 255.0), 0, 255);
        }
        init = true;
    }
    return lut[std::clamp((int)(v * 4095.0f + 0.5f), 0, 4095)];
}

// The danger ramp: deep red -> orange -> yellow -> white (linear light).
glm::vec3 heat(float t)
{
    t = std::clamp(t, 0.0f, 1.0f);
    const glm::vec3 c0(0.18f, 0.005f, 0.004f), c1(0.85f, 0.10f, 0.012f), c2(1.0f, 0.55f, 0.06f), c3(1.0f, 0.95f, 0.65f);
    if (t < 0.4f)
        return glm::mix(c0, c1, t / 0.4f);
    if (t < 0.75f)
        return glm::mix(c1, c2, (t - 0.4f) / 0.35f);
    return glm::mix(c2, c3, (t - 0.75f) / 0.25f);
}

void fmtUtc(double tJ2000, char *buf, size_t n)
{
    const time_t unixSim = (time_t)std::floor(tJ2000) + 946728000;
    const struct tm *u = gmtime(&unixSim);
    if (!u)
        snprintf(buf, n, "--");
    else
        snprintf(buf, n, "%04d-%02d-%02d %02d:%02d UTC", u->tm_year + 1900, u->tm_mon + 1, u->tm_mday, u->tm_hour, u->tm_min);
}

// 1,234 / 45.6k / 1.23M
void fmtCount(double v, char *b, size_t n)
{
    if (v >= 1.0e9)
        snprintf(b, n, "%.2fB", v / 1.0e9);
    else if (v >= 1.0e6)
        snprintf(b, n, "%.2fM", v / 1.0e6);
    else if (v >= 1.0e5)
        snprintf(b, n, "%.0fk", v / 1.0e3);
    else if (v >= 1.0e4)
        snprintf(b, n, "%.1fk", v / 1.0e3);
    else
        snprintf(b, n, "%.0f", v);
}

void fmtAgo(double s, char *b, size_t n)
{
    if (s < 120.0)
        snprintf(b, n, "%.0f s ago", s);
    else if (s < 7200.0)
        snprintf(b, n, "%.0f min ago", s / 60.0);
    else if (s < 3.0 * 86400.0)
        snprintf(b, n, "%.1f h ago", s / 3600.0);
    else
        snprintf(b, n, "%.1f days ago", s / 86400.0);
}

glm::dvec3 eciToEcef(const glm::dvec3 &v, double t)
{
    const double era = earthRotationAngle(t), c = std::cos(era), s = std::sin(era);
    return glm::dvec3(v.x * c + v.y * s, -v.x * s + v.y * c, v.z);
}

Clay_String cs(const char *s) { return Clay_String{false, (int32_t)strlen(s), s}; }
} // namespace

// ── Data ────────────────────────────────────────────────────────────────────────────────────────
void SatelliteSim::kesslerTrackerTables()
{
    if (trTablesGen == kCountsLiveGen && !trDebDen.empty())
        return;
    trTablesGen = kCountsLiveGen;
    constexpr int NA = KCounts::kAlt, NI = KCounts::kInc, NL = kTrLat;
    trDebDen.assign((size_t)NA * NL, 0.0f);
    trWorkDen.assign((size_t)NA * NL, 0.0f);
    // The share of an orbit of inclination i spent between latitudes p1 < p2: F(p2) - F(p1), F = 1/2 + asin(sin p /
    // sin i) / pi inside +-i (circular orbits: a uniform argument of latitude).
    static double kern[NI][NL];
    static bool kInit = false;
    if (!kInit)
    {
        for (int ii = 0; ii < NI; ++ii)
        {
            const double inc = glm::radians(2.5 + 5.0 * ii), si = std::sin(inc);
            auto F = [&](double p) {
                if (p <= -inc)
                    return 0.0;
                if (p >= inc)
                    return 1.0;
                return 0.5 + std::asin(std::clamp(std::sin(p) / si, -1.0, 1.0)) / glm::pi<double>();
            };
            for (int jl = 0; jl < NL; ++jl)
            {
                const double p1 = glm::radians(-90.0 + 5.0 * jl), p2 = p1 + glm::radians(5.0);
                kern[ii][jl] = F(p2) - F(p1);
            }
        }
        kInit = true;
    }
    trWorstChance = 0.0f;
    for (int ia = 0; ia < NA; ++ia)
    {
        const double r1 = 6371.0 + kTrAlt0Km + kTrDAltKm * ia, r2 = r1 + kTrDAltKm;
        const double shellV = 4.0 / 3.0 * glm::pi<double>() * (r2 * r2 * r2 - r1 * r1 * r1); // km^3
        for (int jl = 0; jl < NL; ++jl)
        {
            const double p1 = glm::radians(-90.0 + 5.0 * jl), p2 = p1 + glm::radians(5.0);
            const double vol = shellV * 0.5 * (std::sin(p2) - std::sin(p1));
            double d = 0.0, w = 0.0;
            for (int ii = 0; ii < NI; ++ii)
            {
                d += (double)kCountsLive.deb[ia * NI + ii] * kern[ii][jl];
                w += (double)kCountsLive.work[ia * NI + ii] * kern[ii][jl];
            }
            trDebDen[(size_t)ia * NL + jl] = (float)(d / vol);
            trWorkDen[(size_t)ia * NL + jl] = (float)(w / vol);
            const float ch = (float)(1.0 - std::exp(-d / vol * kTrHitK));
            if (ch > trWorstChance)
            {
                trWorstChance = ch;
                trWorstAltKm = (float)(kTrAlt0Km + kTrDAltKm * (ia + 0.5));
                trWorstLatDeg = (float)(-87.5 + 5.0 * jl);
            }
        }
    }
    // Collisions per day: at most ~120 bins (a week or more each past ~4 months).
    const size_t nd = kCountsLive.colDay.size();
    trPlotBin = std::max(1, (int)((nd + 119) / 120));
    const size_t nb = (nd + (size_t)trPlotBin - 1) / (size_t)trPlotBin;
    std::vector<uint32_t> a(nb, 0u), c(nb, 0u);
    for (size_t d = 0; d < nd; ++d)
    {
        a[d / (size_t)trPlotBin] += kCountsLive.colDay[d];
        c[d / (size_t)trPlotBin] += kCountsLive.catDay[d];
    }
    trPlotPeak = 0;
    trPlotPeakDay = 0;
    for (size_t b = 0; b < nb; ++b)
        if (a[b] > trPlotPeak)
        {
            trPlotPeak = a[b];
            trPlotPeakDay = (int)(b * (size_t)trPlotBin);
        }
    trPlotX.resize(nb);
    trPlotY.resize(nb);
    trPlotY2.resize(nb);
    const float pk = std::max(1.0f, (float)trPlotPeak);
    for (size_t b = 0; b < nb; ++b)
    {
        trPlotX[b] = nb > 1 ? (float)b / (float)(nb - 1) : 0.5f;
        trPlotY[b] = 0.04f + 0.9f * (float)a[b] / pk;
        trPlotY2[b] = 0.04f + 0.9f * (float)c[b] / pk;
    }
}

std::string SatelliteSim::kesslerEventTitle(const kessler::Event &e) const
{
    auto name = [&](uint32_t ref) -> std::string {
        if (ref == kessler::kNoRef)
            return "?";
        if (ref == kessler::kSmallRef)
            return "a 10-cm fragment";
        if (ref & kessler::kPoolBit)
            return "a piece of debris";
        std::string s = satLabel((int)ref);
        const size_t p = s.find(" (dead)");
        if (p != std::string::npos)
            s.erase(p);
        return s;
    };
    char b[320];
    switch (e.kind)
    {
    case kessler::EV_COLLISION:
        snprintf(b, sizeof(b), "%s x %s", name(e.a).c_str(), name(e.b).c_str());
        break;
    case kessler::EV_EXPLOSION:
        snprintf(b, sizeof(b), "%s exploded", name(e.a).c_str());
        break;
    case kessler::EV_ASAT:
        snprintf(b, sizeof(b), "ASAT strike on %s", name(e.a).c_str());
        break;
    default:
        snprintf(b, sizeof(b), "%s", name(e.a).c_str());
        break;
    }
    return b;
}

bool SatelliteSim::kesslerWatchEvent(uint32_t ev, std::string &msg)
{
    FPROF_SCOPE("kesslerWatchEvent");
    if (ev >= kEvents.size())
    {
        msg = "no such breakup";
        return false;
    }
    const kessler::Event &e = kEvents[ev];
    const glm::dvec3 B(e.pos[0], e.pos[1], e.pos[2]), V(e.vel[0], e.vel[1], e.vel[2]);
    if (glm::length(B) < kR)
    {
        msg = "that breakup has no place recorded";
        return false;
    }
    if (followActive)
        stopFollow();
    if (trackActive)
        stopTrack();
    // (2026-10-24) Just after it: the objects already broken up, the debris and its dust spreading (it was 8 s before:
    // an empty sky). A piece of it is selected and tracked once the mirror has it (kesslerWatchPieceTick).
    const double t0 = e.t + 3.0;
    const double days = std::floor(t0 / 86400.0);
    simDayJ2000 = (int64_t)days;
    simSecInDay = t0 - days * 86400.0;
    trailClearPending = true;
    timePaused = false;
    timeScaleIdx = 0;
    timeDir = 1.0f;
    // The eye: 80 km beside the orbit (across the velocity) and 25 km BELOW it, Earth-fixed at the breakup's moment, so
    // the debris is seen against black space, not the bright Earth (15 km above: lost against the clouds below).
    const glm::dvec3 U = glm::normalize(B);
    glm::dvec3 A = glm::length(V) > 1.0 ? glm::normalize(V) : glm::dvec3(0, 0, 1);
    glm::dvec3 C = glm::cross(A, U);
    C = glm::length(C) > 1e-6 ? glm::normalize(C) : glm::dvec3(1, 0, 0);
    const glm::dvec3 eyeEci = B + C * 80000.0 - U * 25000.0;
    const glm::dvec3 eye = eciToEcef(eyeEci, e.t), tgt = eciToEcef(B, e.t);
    const glm::dvec3 g = glm::normalize(eye);
    setObserverLatLon(glm::degrees(std::asin(glm::clamp(g.z, -1.0, 1.0))), glm::degrees(std::atan2(g.y, g.x)));
    obsHeightOffset = (float)(glm::length(eye) - kR);
    const glm::dvec3 up = g;
    glm::dvec3 east = glm::cross(glm::dvec3(0, 0, 1), up);
    east = glm::length(east) > 1e-9 ? glm::normalize(east) : glm::dvec3(1, 0, 0);
    const glm::dvec3 north = glm::cross(up, east);
    const glm::dvec3 rn = glm::normalize(tgt - eye);
    aimCameraAzEl((float)glm::degrees(std::atan2(glm::dot(rn, east), glm::dot(rn, north))),
                  (float)glm::degrees(std::asin(glm::clamp(glm::dot(rn, up), -1.0, 1.0))));
    camera.fovYDeg = 70.0f;
    updatePositions(t0);
    skyTaaHistValid = false;
    cv2HistoryValid = false;
    selectedSatIndex = -1;
    selectedPlanetIndex = -1;
    selTransientKey = 0;
    kWatchPieceEv = (int64_t)ev;
    kWatchPieceUntil = e.t + 120.0;
    kWatchPieceScan = 0;
    char b[400];
    snprintf(b, sizeof(b), "%s: the debris 80 km away (%.0f km up)", kesslerEventTitle(e).c_str(), (glm::length(B) - kR) / 1000.0);
    msg = b;
    return true;
}

// A Watch's breakup: select its first record once the mirror shows it, and lock the camera on it. The pool is scanned
// in 400k-slot pieces a frame (5M slots at once would be a hitch).
void SatelliteSim::kesslerWatchPieceTick()
{
    FPROF_SCOPE("kesslerWatchPieceTick");
    if (kWatchPieceEv < 0)
        return;
    const double t = (double)simDayJ2000 * 86400.0 + simSecInDay;
    if (t > kWatchPieceUntil || t < kWatchPieceUntil - 600.0 || selectedSatIndex >= 0)
    {
        kWatchPieceEv = -1;
        return;
    }
    const uint32_t n = (uint32_t)kPool.size();
    const uint32_t end = std::min<uint32_t>(n, kWatchPieceScan + 400000u);
    for (uint32_t j = kWatchPieceScan; j < end; ++j)
        if (kPool[j].event == (uint32_t)kWatchPieceEv)
        {
            const int idx = (int)poolIdx0() + (int)j;
            selectSatellite(idx);
            // (The eye stays where the Watch put it, beside the breakup's recorded place: since kCacheVersion 17 an
            // engine collision's debris and dust start there. kWatchPieceOffKm: the piece's distance from that place
            // carried to now on the breakup's orbit, harness `kessler state`.)
            {
                const glm::dvec3 P = satOrbitStateAt(orbitElemsOf(satOrbits[(size_t)idx]), t).posEci;
                const kessler::Event &e = kEvents[(size_t)kWatchPieceEv];
                kessler::State s0{{e.pos[0], e.pos[1], e.pos[2]}, {e.vel[0], e.vel[1], e.vel[2]}};
                kessler::Elems el = kessler::elemsFromState(s0);
                el.M += std::sqrt(kessler::kGM / (el.a * el.a * el.a)) * (t - e.t);
                kWatchPieceOffKm = glm::length(P - kessler::stateFromElems(el).r) / 1000.0;
            }
            startTrack();
            kWatchPieceEv = -1;
            return;
        }
    kWatchPieceScan = end >= n ? 0u : end;
}

// ── The globe ───────────────────────────────────────────────────────────────────────────────────
bool SatelliteSim::kesslerTrackerEnsureImage(UIRenderer &ui)
{
    if (trImg != VK_NULL_HANDLE)
        return trUiId != 0;
    if (!ctx_ || trImageFailed)
        return false;
    VulkanContext &ctx = *ctx_;
    ctx.createImage(kTrN, kTrN, VK_FORMAT_R8G8B8A8_SRGB, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, trImg, trMem);
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = trImg;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = VK_FORMAT_R8G8B8A8_SRGB;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCreateImageView(ctx.device, &vi, nullptr, &trView);
    VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    si.magFilter = si.minFilter = VK_FILTER_LINEAR;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    vkCreateSampler(ctx.device, &si, nullptr, &trSampler);
    const VkDeviceSize bytes = (VkDeviceSize)kTrN * kTrN * 4;
    // The raster's CPU copy; each upload takes this frame's staging (ctx.stagingAlloc: frames in flight).
    trStageMapped = std::calloc((size_t)bytes, 1);
    // Transparent until the first raster (sampled from the first frame the tab is drawn).
    VkCommandBuffer cmd = ctx.beginOneTimeCommands();
    ctx.imageBarrier(cmd, trImg, 0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                     VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    const VkClearColorValue clear{{0.0f, 0.0f, 0.0f, 0.0f}};
    const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(cmd, trImg, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear, 1, &range);
    ctx.imageBarrier(cmd, trImg, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    ctx.endOneTimeCommands(cmd);
    trUiId = ui.registerImage(ctx.device, trView, trSampler);
    if (trUiId == 0)
    {
        trImageFailed = true;
        Log::line("kessler tracker: no free UI image slot for the globe");
    }
    return trUiId != 0;
}

void SatelliteSim::kesslerTrackerRecordUpload(VkCommandBuffer cmd)
{
    if (!trUploadPending || trImg == VK_NULL_HANDLE || !ctx_)
        return;
    trUploadPending = false;
    VulkanContext &ctx = *ctx_;
    ctx.imageBarrier(cmd, trImg, VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    const VkDeviceSize bytes = (VkDeviceSize)kTrN * kTrN * 4;
    const VulkanContext::StagingAlloc st = ctx.stagingAlloc(bytes);
    std::memcpy(st.ptr, trStageMapped, (size_t)bytes);
    VkBufferImageCopy region{};
    region.bufferOffset = st.off;
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {(uint32_t)kTrN, (uint32_t)kTrN, 1};
    vkCmdCopyBufferToImage(cmd, st.buf, trImg, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    ctx.imageBarrier(cmd, trImg, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
}

void SatelliteSim::kesslerTrackerDestroy(VkDevice device)
{
    std::free(trStageMapped);
    trStageMapped = nullptr;
    if (trStage)
        vkDestroyBuffer(device, trStage, nullptr);
    if (trStageMem)
        vkFreeMemory(device, trStageMem, nullptr);
    if (trSampler)
        vkDestroySampler(device, trSampler, nullptr);
    if (trView)
        vkDestroyImageView(device, trView, nullptr);
    if (trImg)
        vkDestroyImage(device, trImg, nullptr);
    if (trMem)
        vkFreeMemory(device, trMem, nullptr);
    trStage = VK_NULL_HANDLE;
    trStageMem = VK_NULL_HANDLE;
    trSampler = VK_NULL_HANDLE;
    trView = VK_NULL_HANDLE;
    trImg = VK_NULL_HANDLE;
    trMem = VK_NULL_HANDLE;
}

// The raster: the Earth (the day map, lit by the real Sun, the night lights on the dark side), the air's rim, the
// altitude rings, and the shells' danger round it. View: from the equator at longitude trViewLonDeg, north up.
void SatelliteSim::kesslerTrackerRenderGlobe()
{
    FPROF_SCOPE("kesslerTrackerRenderGlobe");
    if (!trStageMapped)
        return;
    const double t = (double)simDayJ2000 * 86400.0 + simSecInDay;
    const double lc = glm::radians(trViewLonDeg);
    const glm::dvec3 V(std::cos(lc), std::sin(lc), 0.0), E(-std::sin(lc), std::cos(lc), 0.0), Nz(0, 0, 1);
    const glm::dvec3 sun = glm::normalize(eciToEcef(glm::dvec3(sunDirECI), t));
    uint8_t *px = static_cast<uint8_t *>(trStageMapped);
    const double half = kTrN * 0.5;
    const double ringR[4] = {1.0 + kTrAltK * 500.0, 1.0 + kTrAltK * 1000.0, 1.0 + kTrAltK * 1500.0, 1.0 + kTrAltK * 2000.0};
    const double pxW = kTrExtent / half; // Earth radii per texel
    constexpr int NL = kTrLat;
    const bool haveTables = !trDebDen.empty();
    for (int j = 0; j < kTrN; ++j)
        for (int i = 0; i < kTrN; ++i)
        {
            const double x = (i + 0.5 - half) / half * kTrExtent, y = (half - j - 0.5) / half * kTrExtent;
            const double rho = std::sqrt(x * x + y * y);
            glm::vec3 col(0.0f);
            float a = 0.0f;
            if (rho <= 1.0)
            {
                const double z = std::sqrt(std::max(0.0, 1.0 - rho * rho));
                const glm::dvec3 P = x * E + y * Nz + z * V;
                const double lat = std::asin(std::clamp(P.z, -1.0, 1.0)), lon = std::atan2(P.y, P.x);
                glm::vec3 day(0.08f, 0.1f, 0.14f);
                if (!earthDayCpu.empty())
                {
                    const int cx = std::clamp((int)((lon + glm::pi<double>()) / glm::two_pi<double>() * earthDayCpuW), 0, earthDayCpuW - 1);
                    const int cy = std::clamp((int)((0.5 * glm::pi<double>() - lat) / glm::pi<double>() * earthDayCpuH), 0, earthDayCpuH - 1);
                    const uint8_t *d = &earthDayCpu[((size_t)cy * earthDayCpuW + cx) * 3];
                    day = glm::vec3(srgbToLin(d[0]), srgbToLin(d[1]), srgbToLin(d[2]));
                }
                const float mu = (float)glm::dot(P, sun);
                const float lit = glm::smoothstep(-0.08f, 0.3f, mu);
                // Stylised: a dim, slightly desaturated Earth so the shells and markers read over it.
                const float lum = 0.3f * day.r + 0.55f * day.g + 0.15f * day.b;
                day = glm::mix(glm::vec3(lum), day, 0.75f);
                col = day * (0.05f + 0.6f * lit);
                if (!earthNightCpu.empty() && lit < 0.9f)
                {
                    const int nx = std::clamp((int)((lon + glm::pi<double>()) / glm::two_pi<double>() * earthNightCpuW), 0, earthNightCpuW - 1);
                    const int ny = std::clamp((int)((0.5 * glm::pi<double>() - lat) / glm::pi<double>() * earthNightCpuH), 0, earthNightCpuH - 1);
                    const float nl = std::max(0.0f, earthNightCpu[(size_t)ny * earthNightCpuW + nx] / 255.0f - 0.07f);
                    col += glm::vec3(1.0f, 0.62f, 0.28f) * (nl * 0.9f * (1.0f - lit));
                }
                col *= (float)(0.55 + 0.45 * z); // limb darkening
                a = 1.0f;
                // the edge, anti-aliased
                a *= (float)std::clamp((1.0 - rho) / pxW + 0.5, 0.0, 1.0);
            }
            if (rho > 1.0 - pxW && haveTables)
            {
                // The shells as a polar plot round the limb: altitude by radius, latitude by angle.
                glm::vec3 sc(0.0f);
                float sa = 0.0f;
                const double hKm = (rho - 1.0) / kTrAltK;
                const double latD = glm::degrees(std::asin(std::clamp(y / std::max(rho, 1e-9), -1.0, 1.0)));
                const double fa = (hKm - kTrAlt0Km) / kTrDAltKm - 0.5;
                const double fl = (latD + 90.0) / 5.0 - 0.5;
                if (fa > -1.0 && fa < KCounts::kAlt)
                {
                    const int a0 = (int)std::floor(fa), l0 = (int)std::floor(fl);
                    const float ta = (float)(fa - a0), tl = (float)(fl - l0);
                    auto at = [&](const std::vector<float> &v, int ia, int il) {
                        if (ia < 0 || ia >= KCounts::kAlt)
                            return 0.0f;
                        il = std::clamp(il, 0, NL - 1);
                        return v[(size_t)ia * NL + il];
                    };
                    auto bil = [&](const std::vector<float> &v) {
                        return glm::mix(glm::mix(at(v, a0, l0), at(v, a0, l0 + 1), tl),
                                        glm::mix(at(v, a0 + 1, l0), at(v, a0 + 1, l0 + 1), tl), ta);
                    };
                    const float dd = bil(trDebDen), wd = bil(trWorkDen);
                    // Working satellites: blue, by density (1e-8 .. 1e-4 per km^3; a mega-shell's core is the top).
                    if (wd > 0.0f)
                    {
                        const float w = std::clamp(((float)std::log10(std::max(wd, 1e-12f)) + 8.0f) / 4.0f, 0.0f, 1.0f);
                        sc = glm::vec3(0.05f, 0.16f, 0.55f) * (0.35f + 0.9f * w);
                        sa = 0.6f * glm::smoothstep(0.0f, 0.5f, w);
                    }
                    // The danger: the chance a year a 10 m^2 satellite is hit (1 in 10,000 .. 1 in 10 and up).
                    if (dd > 0.0f)
                    {
                        const float ch = 1.0f - std::exp(-dd * (float)kTrHitK);
                        const float h = std::clamp(((float)std::log10(std::max(ch, 1e-12f)) + 4.0f) / 3.0f, 0.0f, 1.0f);
                        const float ha = glm::smoothstep(0.0f, 0.12f, h) * (0.55f + 0.45f * h);
                        sc = glm::mix(sc, heat(h) * 1.6f, ha);
                        sa = sa + (1.0f - sa) * ha;
                    }
                }
                // The air's thin rim.
                const float rim = (float)std::exp(-std::max(0.0, rho - 1.0) / (pxW * 2.5));
                if (rho > 1.0)
                {
                    sc = glm::mix(sc, glm::vec3(0.2f, 0.45f, 0.9f), rim * 0.6f);
                    sa = std::max(sa, rim * 0.55f);
                    // The altitude rings (500 / 1000 / 1500 / 2000 km): faint grey lines.
                    for (double rr : ringR)
                    {
                        const float g = (float)std::clamp(1.0 - std::abs(rho - rr) / (pxW * 0.8), 0.0, 1.0);
                        if (g > 0.0f)
                        {
                            sc = glm::mix(sc, glm::vec3(0.45f), g * 0.35f);
                            sa = std::max(sa, g * 0.3f);
                        }
                    }
                }
                // over the disc only its rim shows; outside, the shells
                col = col * a + sc * sa * (1.0f - a);
                a = a + sa * (1.0f - a);
                if (a > 1e-4f)
                    col /= a;
            }
            uint8_t *o = &px[((size_t)j * kTrN + i) * 4];
            o[0] = linToSrgb(col.r);
            o[1] = linToSrgb(col.g);
            o[2] = linToSrgb(col.b);
            o[3] = (uint8_t)std::clamp((int)std::lround(a * 255.0f), 0, 255);
        }
    trUploadPending = true;

    // The markers: the latest breakups at their place (altitude exaggerated), front or behind the globe.
    trMarkers.clear();
    for (int k = 0; k < kCountsLive.nRecent; ++k)
    {
        const uint32_t ev = kCountsLive.recent[k];
        if (ev >= kEvents.size())
            continue;
        const kessler::Event &e = kEvents[ev];
        const glm::dvec3 B(e.pos[0], e.pos[1], e.pos[2]);
        const double r = glm::length(B);
        if (r < kR)
            continue;
        const glm::dvec3 d = glm::normalize(eciToEcef(B, e.t));
        const double rr = 1.0 + kTrAltK * (r - kR) / 1000.0;
        const glm::dvec3 P = d * rr;
        const double sx = glm::dot(P, E), sy = glm::dot(P, Nz), sz = glm::dot(P, V);
        TrMarker m;
        m.x = (float)(0.5 + 0.5 * sx / kTrExtent);
        m.y = (float)(0.5 - 0.5 * sy / kTrExtent);
        m.front = sz >= 0.0 || (sx * sx + sy * sy) > 1.0;
        m.ev = ev;
        m.rank = k + 1;
        trMarkers.push_back(m);
    }
    trRenderedLon = trViewLonDeg;
    trRenderedGen = kCountsLiveGen;
    trRenderedSunLon = glm::degrees(std::atan2(sun.y, sun.x));
    trRenderWall = wallNow();
}

nlohmann::json SatelliteSim::kesslerTrackerJson()
{
    kesslerStateCached(); // keeps the counts walk going
    kesslerTrackerTables();
    nlohmann::json j;
    j["counts_gen"] = kCountsLiveGen;
    j["watch_piece_off_km"] = kWatchPieceOffKm;
    j["counts_valid"] = kCountsLiveValid;
    j["working"] = kCountsLive.working;
    j["reentered"] = kCountsLive.reentered;
    j["explosions"] = kCountsLive.explosions;
    j["collision_days"] = kCountsLive.colDay.size();
    j["plot_bin_days"] = trPlotBin;
    j["peak_collisions_per_bin"] = trPlotPeak;
    j["peak_day"] = trPlotPeakDay;
    j["worst_hit_chance_per_year"] = trWorstChance;
    j["worst_alt_km"] = trWorstAltKm;
    j["worst_lat_deg"] = trWorstLatDeg;
    j["globe_ui_id"] = trUiId;
    j["globe_lon_deg"] = trViewLonDeg;
    nlohmann::json rc = nlohmann::json::array();
    const double now = (double)simDayJ2000 * 86400.0 + simSecInDay;
    for (int k = 0; k < kCountsLive.nRecent; ++k)
    {
        const uint32_t ev = kCountsLive.recent[k];
        if (ev >= kEvents.size())
            continue;
        const kessler::Event &e = kEvents[ev];
        const glm::dvec3 B(e.pos[0], e.pos[1], e.pos[2]);
        const glm::dvec3 d = glm::normalize(eciToEcef(B, e.t));
        rc.push_back({{"event", ev}, {"kind", (int)e.kind}, {"user", e.origin == kessler::ORIGIN_USER},
                      {"title", kesslerEventTitle(e)}, {"hours_ago", (now - e.t) / 3600.0},
                      {"alt_km", (glm::length(B) - kR) / 1000.0}, {"lat_deg", glm::degrees(std::asin(d.z))},
                      {"lon_deg", glm::degrees(std::atan2(d.y, d.x))}, {"vrel_kms", e.vRel / 1000.0}});
    }
    j["recent"] = rc;
    j["markers"] = trMarkers.size();
    return j;
}

// ── The tab ─────────────────────────────────────────────────────────────────────────────────────
void SatelliteSim::buildKesslerTrackerTab(const UIInput &inp, UIRenderer &ui, float bodyW)
{
    FPROF_SCOPE("buildKesslerTrackerTab");
    const double now = (double)simDayJ2000 * 86400.0 + simSecInDay;
    auto text = [&](const char *s, Clay_Color c, int size, bool wrap = false) {
        CLAY_TEXT(cs(s), CLAY_TEXT_CONFIG({.textColor = c, .fontSize = fs(size),
                                           .wrapMode = wrap ? CLAY_TEXT_WRAP_WORDS : CLAY_TEXT_WRAP_NONE}));
    };
    if (!kesslerEnabled)
    {
        uiSection("TrSec", 0, "THE SUN'S FURY");
        text("The Kessler scenario is off: no storm, no cascade. Turn it on in Settings > Launches.", Pal::textDim, 11, true);
        if (uiButton(inp, ui, "TrOpenSettings", 0, "Open the settings", "Settings > Launches"))
        {
            settingsChrome.open = true;
            settingsActiveTab = 15;
        }
        return;
    }
    const nlohmann::json &js = kesslerStateCached(); // (also keeps the counts walk going while the tab is open)
    kesslerTrackerTables();
    const double days = (now - kesslerEpoch()) / 86400.0;
    const KCounts &c = kCountsLive;

    // ── The headline strip ───────────────────────────────────────────────────────────────────────
    CLAY(CLAY_ID("TrHead"), {.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0)},
                                        .padding = {10, 10, 7, 8},
                                        .childGap = 8,
                                        .childAlignment = {.y = CLAY_ALIGN_Y_CENTER},
                                        .layoutDirection = CLAY_LEFT_TO_RIGHT},
                             .backgroundColor = Pal::furyBg,
                             .cornerRadius = CLAY_CORNER_RADIUS(4),
                             .border = {.color = Pal::furyBorder, .width = CLAY_BORDER_ALL(2)}})
    {
        CLAY(CLAY_ID("TrHeadL"), {.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0)},
                                             .childGap = 2,
                                             .layoutDirection = CLAY_TOP_TO_BOTTOM}})
        {
            text("THE SUN'S FURY", Pal::furyText, 16);
            const std::string hold = kesslerStatusLine();
            if (!hold.empty())
                text(msStr("%s", hold.c_str()), Pal::textWarn, 11, true);
            else if (days < 0.0)
                text("Kessler tracker: the storm has not arrived", Pal::furyText, 11, true);
            else
                text(msStr("Kessler tracker: simulated to day %.1f%s", js["frontier_days"].get<double>(),
                           kWorkerDone ? " (done)" : ""),
                     Pal::furyText, 11, true);
        }
        if (days < 0.0)
        {
            const long long s = (long long)std::floor(-days * 86400.0);
            if (s >= 86400)
                text(msStr("T- %lldd %02lldh", s / 86400, (s % 86400) / 3600), Pal::furyText, 18);
            else
                text(msStr("T- %lldh %02lldm %02llds", s / 3600, (s % 3600) / 60, s % 60), Pal::furyText, 18);
        }
        else
            text(msStr("DAY %.1f", days), Pal::furyText, 20);
    }

    // ── The counts ───────────────────────────────────────────────────────────────────────────────
    char b0[32], b1[32], b2[32], b3[32];
    auto row = [&](uint32_t id, const std::function<void()> &f) {
        CLAY(CLAY_IDI("TrTiles", id), {.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0)},
                                                  .childGap = 5,
                                                  .layoutDirection = CLAY_LEFT_TO_RIGHT}})
        {
            f();
        }
    };
    if (days < 0.0)
    {
        fmtCount((double)c.working, b0, sizeof(b0));
        char ua[48];
        fmtUtc(kesslerEpoch(), ua, sizeof(ua));
        row(0, [&]() {
            uiStatTile("TrTile", 0, "WORKING SATELLITES", msStr("%s", b0), Pal::volValue);
            uiStatTile("TrTile", 1, "CME ARRIVES", msStr("%.10s", ua), Pal::furyText);
            uiStatTile("TrTile", 2, "KP NOW", msStr("%.1f", spaceWeather_.kp), Pal::volValue);
        });
        text("When the storm arrives, a share of these satellites die. The dead tumble, drift out of their formations and "
             "collide; every collision is a debris cloud that feeds the next. Blue on the globe is where they fly today.",
             Pal::textPrimary, 11, true);
    }
    else
    {
        fmtCount((double)c.col, b0, sizeof(b0));
        fmtCount((double)c.cat, b1, sizeof(b1));
        fmtCount((double)c.fragReal, b2, sizeof(b2));
        fmtCount(c.smallMade, b3, sizeof(b3));
        row(0, [&]() {
            uiStatTile("TrTile", 0, "COLLISIONS", msStr("%s", b0), Pal::trkLine);
            uiStatTile("TrTile", 1, "CATASTROPHIC", msStr("%s", b1), Pal::trkLineCat);
            uiStatTile("TrTile", 2, "PIECES >= 1 M", msStr("%s", b2), Pal::volValue);
            uiStatTile("TrTile", 3, "FRAGMENTS 10 CM+", msStr("%s", b3), Pal::volValue);
        });
        fmtCount((double)c.dead, b0, sizeof(b0));
        fmtCount((double)(c.rosterGone - std::min(c.rosterGone, c.reentered)), b1, sizeof(b1));
        fmtCount((double)c.reentered, b2, sizeof(b2));
        fmtCount((double)c.working, b3, sizeof(b3));
        row(1, [&]() {
            uiStatTile("TrTile", 4, "DEAD, TUMBLING", msStr("%s", b0), Pal::textWarn);
            uiStatTile("TrTile", 5, "DESTROYED", msStr("%s", b1), Pal::furyText);
            uiStatTile("TrTile", 6, "RE-ENTERED", msStr("%s", b2), Pal::volValue);
            uiStatTile("TrTile", 7, "STILL WORKING", msStr("%s", b3), Pal::trkWork);
        });
        if (!kCountsLiveValid)
            text("Counting...", Pal::textHint, 10);
    }

    // ── The globe and its legend ─────────────────────────────────────────────────────────────────
    uiSection("TrSec", 1, "DEBRIS SHELLS");
    const bool haveImg = kesslerTrackerEnsureImage(ui);
    if (trViewLonDeg > 1e8)
        trViewLonDeg = obsLonDeg;
    const bool wide = bodyW >= 470.0f;
    const float G = std::floor(wide ? std::clamp(bodyW * 0.5f, 180.0f, 320.0f) : std::clamp(bodyW - 8.0f, 160.0f, 320.0f));
    trGlobePx = G;
    // Drag to turn it; a press on a marker picks its row.
    const Clay_ElementId gid = CLAY_ID("TrGlobe");
    const Clay_ElementData gd = Clay_GetElementData(gid);
    const bool gOk = gd.found && gd.boundingBox.width > 1.0f;
    trHoverMarker = -1;
    if (gOk)
    {
        const float mx = (inp.mouseX - gd.boundingBox.x) / gd.boundingBox.width;
        const float my = (inp.mouseY - gd.boundingBox.y) / gd.boundingBox.height;
        const bool over = mx >= 0.0f && mx <= 1.0f && my >= 0.0f && my <= 1.0f;
        if (over)
        {
            float best = 9.0f / gd.boundingBox.width;
            for (size_t k = 0; k < trMarkers.size(); ++k)
            {
                const float d = std::hypot(trMarkers[k].x - mx, trMarkers[k].y - my);
                if (d < best)
                {
                    best = d;
                    trHoverMarker = (int)k;
                }
            }
        }
        if (over && inp.lmbPressed)
        {
            if (trHoverMarker >= 0)
                trSelEvent = trMarkers[(size_t)trHoverMarker].rank - 1;
            else
            {
                trDrag = true;
                trDragX = inp.mouseX;
            }
        }
        if (trDrag && inp.lmbDown)
        {
            trViewLonDeg -= (inp.mouseX - trDragX) * 0.6;
            trViewLonDeg = std::remainder(trViewLonDeg, 360.0);
            trDragX = inp.mouseX;
            ui.holdDragScroll();
        }
        else
            trDrag = false;
    }
    // Re-raster when its inputs changed: a published walk, the view turned, the Sun moved ~1 deg, at most 4 / s.
    if (haveImg)
    {
        const double sunLon = [&] {
            const glm::dvec3 s = eciToEcef(glm::dvec3(sunDirECI), now);
            return glm::degrees(std::atan2(s.y, s.x));
        }();
        const bool changed = trRenderedGen != kCountsLiveGen || std::abs(trRenderedLon - trViewLonDeg) > 0.25 ||
                             std::abs(std::remainder(trRenderedSunLon - sunLon, 360.0)) > 1.0;
        if (changed && (trDrag || wallNow() - trRenderWall > 0.25))
            kesslerTrackerRenderGlobe();
    }
    // The overlay: markers (numbered, the latest pulsing), "you", the rings' altitudes.
    trQuads.clear();
    trRuns.clear();
    trRunText.clear();
    {
        const float pulse = 0.5f + 0.5f * (float)std::sin(glfwGetTime() * 5.0);
        auto ring = [&](float x, float y, float r, float th, glm::vec4 col) {
            trQuads.push_back({x - r, y - r, 2.0f * r, th, col});
            trQuads.push_back({x - r, y + r - th, 2.0f * r, th, col});
            trQuads.push_back({x - r, y - r + th, th, 2.0f * r - 2.0f * th, col});
            trQuads.push_back({x + r - th, y - r + th, th, 2.0f * r - 2.0f * th, col});
        };
        auto label = [&](float x, float y, const char *s, glm::vec4 col) {
            (void)col;
            trRunText.emplace_back(s);
            trRuns.push_back({trRunText.back().c_str(), (int)trRunText.back().size(), x, y});
        };
        for (const TrMarker &m : trMarkers)
        {
            if (m.ev >= kEvents.size())
                continue; // (the world restarted since the raster)
            const kessler::Event &e = kEvents[m.ev];
            const bool user = e.origin == kessler::ORIGIN_USER;
            glm::vec4 col = palV(user ? Pal::trkMarkerUser : Pal::trkMarker, m.front ? 1.0f : 0.35f);
            const float x = m.x * G, y = m.y * G;
            const bool sel = trSelEvent == m.rank - 1 || trHoverMarker == (int)(&m - trMarkers.data());
            const float r = (m.rank == 1 ? 5.0f + 2.0f * pulse : 4.0f) + (sel ? 2.0f : 0.0f);
            ring(x, y, r, sel ? 2.0f : 1.5f, col);
            trQuads.push_back({x - 1.0f, y - 1.0f, 2.0f, 2.0f, col});
            char nb[8];
            snprintf(nb, sizeof(nb), "%d", m.rank);
            label(x + r + 2.0f, y - r - 2.0f, nb, col);
        }
        // You.
        {
            const double lc = glm::radians(trViewLonDeg), la = glm::radians((double)obsLatDeg), lo = glm::radians((double)obsLonDeg);
            const glm::dvec3 P(std::cos(la) * std::cos(lo), std::cos(la) * std::sin(lo), std::sin(la));
            const glm::dvec3 V(std::cos(lc), std::sin(lc), 0.0), E(-std::sin(lc), std::cos(lc), 0.0);
            if (glm::dot(P, V) > 0.0)
            {
                const float x = (float)(0.5 + 0.5 * glm::dot(P, E) / kTrExtent) * G, y = (float)(0.5 - 0.5 * P.z / kTrExtent) * G;
                trQuads.push_back({x - 2.5f, y - 2.5f, 5.0f, 5.0f, palV(Pal::trkYou)});
            }
        }
        // The rings' altitudes, at the top.
        const float cx = 0.5f * G;
        const int alts[4] = {500, 1000, 1500, 2000};
        for (int k = 0; k < 4; ++k)
        {
            const float y = (float)(0.5 - 0.5 * (1.0 + kTrAltK * alts[k]) / kTrExtent) * G;
            char nb[16];
            snprintf(nb, sizeof(nb), "%d km", alts[k]);
            if (k % 2 == 1) // (500 / 1500 crowd them at this size)
                label(cx + 3.0f, y - (float)fs(9) - 1.0f, nb, glm::vec4(0.7f));
        }
    }
    trMarks.runs = trRuns.data();
    trMarks.runCount = (int)trRuns.size();
    trMarks.quads = trQuads.data();
    trMarks.quadCount = (int)trQuads.size();
    trMarks.fontSize = (float)fs(9);
    trMarks.color = palV(Pal::textDim, 1.0f);
    trImgEl.imageId = trUiId;

    auto legend = [&]() {
        auto swatch = [&](uint32_t id, Clay_Color col, const char *s) {
            CLAY(CLAY_IDI("TrLeg", id), {.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0)},
                                                    .childGap = 6,
                                                    .childAlignment = {.y = CLAY_ALIGN_Y_CENTER},
                                                    .layoutDirection = CLAY_LEFT_TO_RIGHT}})
            {
                CLAY(CLAY_IDI("TrLegSw", id), {.layout = {.sizing = {CLAY_SIZING_FIXED(10), CLAY_SIZING_FIXED(10)}},
                                               .backgroundColor = col,
                                               .cornerRadius = CLAY_CORNER_RADIUS(2)}) {}
                CLAY(CLAY_IDI("TrLegT", id), {.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0)}}})
                {
                    text(s, Pal::textDim, 10, true);
                }
            }
        };
        text("Round the globe: the shells, altitude outward (x1.6), latitude by angle. Drag to turn.", Pal::textDim, 10, true);
        swatch(0, Pal::trkWork, "Working satellites");
        swatch(1, Pal::trkHot, "Danger: the chance a year that a 10 m^2 satellite there is hit (dim: 1 in 10,000; white: 1 in 10 or more)");
        swatch(2, Pal::trkMarker, "The latest breakups (1 = newest)");
        swatch(3, Pal::trkMarkerUser, "Your breakups");
        swatch(4, Pal::trkYou, "You");
        if (trWorstChance > 0.0f)
            text(msStr("Worst shell: %.0f km at %.0f deg %s: %.2g%% a year", trWorstAltKm, std::abs(trWorstLatDeg),
                       trWorstLatDeg >= 0.0f ? "N" : "S", 100.0 * trWorstChance),
                 Pal::trkHot, 11, true);
        else if (days >= 0.0)
            text("No tracked debris yet.", Pal::textHint, 10);
        text("Tracked: dead satellites and pieces of 1 m or more (the 10-cm fragments are a density field not drawn here).",
             Pal::textHint, 10, true);
    };
    CLAY(CLAY_ID("TrGlobeRow"), {.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0)},
                                            .childGap = 10,
                                            .childAlignment = {.y = CLAY_ALIGN_Y_CENTER},
                                            .layoutDirection = wide ? CLAY_LEFT_TO_RIGHT : CLAY_TOP_TO_BOTTOM}})
    {
        CLAY(CLAY_ID("TrGlobeBack"), {.layout = {.sizing = {CLAY_SIZING_FIXED(G), CLAY_SIZING_FIXED(G)}},
                                      .backgroundColor = Pal::insetBg,
                                      .cornerRadius = CLAY_CORNER_RADIUS(4)})
        {
            if (haveImg)
            {
                CLAY(gid, {.layout = {.sizing = {CLAY_SIZING_FIXED(G), CLAY_SIZING_FIXED(G)}},
                           .custom = {.customData = &trImgEl}})
                {
                    CLAY(CLAY_ID("TrGlobeMarks"), {.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0)}},
                                                   .custom = {.customData = &trMarks}}) {}
                }
            }
        }
        CLAY(CLAY_ID("TrLegend"), {.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0)},
                                              .childGap = 4,
                                              .layoutDirection = CLAY_TOP_TO_BOTTOM}})
        {
            legend();
        }
    }
    if (trHoverMarker >= 0 && trHoverMarker < (int)trMarkers.size() && trMarkers[(size_t)trHoverMarker].ev < kEvents.size())
    {
        const kessler::Event &e = kEvents[trMarkers[(size_t)trHoverMarker].ev];
        ui.tooltip(inp, true, msStr("%d. %s", trMarkers[(size_t)trHoverMarker].rank, kesslerEventTitle(e).c_str()), fs(11));
    }

    if (days < 0.0)
        return;

    // ── Collisions per day ───────────────────────────────────────────────────────────────────────
    uiSection("TrSec", 2, trPlotBin > 1 ? "COLLISIONS PER WEEK-OR-SO SINCE THE CME" : "COLLISIONS PER DAY SINCE THE CME");
    if (trPlotX.size() >= 2)
    {
        trSeries[0] = {trPlotX.data(), trPlotY.data(), (int)trPlotX.size(), palV(Pal::trkLine), 1.75f};
        trSeries[1] = {trPlotX.data(), trPlotY2.data(), (int)trPlotX.size(), palV(Pal::trkLineCat), 1.25f};
        trPlot.series = trSeries;
        trPlot.seriesCount = 2;
        CLAY(CLAY_ID("TrPlotBack"), {.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED((float)fs(64))},
                                                .padding = {4, 4, 4, 4}},
                                     .backgroundColor = Pal::insetBg,
                                     .cornerRadius = CLAY_CORNER_RADIUS(4)})
        {
            CLAY(CLAY_ID("TrPlot"), {.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0)}},
                                     .custom = {.customData = &trPlot}}) {}
        }
        text(msStr("Peak %u %s (day %d). Orange: all collisions; red: catastrophic. %s", trPlotPeak,
                   trPlotBin > 1 ? msStr("in %d days", trPlotBin) : "in a day", trPlotPeakDay,
                   trPlotBin > 1 ? msStr("(%d-day bins)", trPlotBin) : ""),
             Pal::textDim, 10, true);
    }
    else
        text(c.col ? "The first day: a few collisions so far." : "No collisions yet (the dead have to drift into each other first).",
             Pal::textHint, 10, true);

    // ── The latest breakups ──────────────────────────────────────────────────────────────────────
    uiSection("TrSec", 3, "LATEST BREAKUPS");
    int watch = -1;
    if (c.nRecent == 0)
        text("None yet.", Pal::textHint, 11);
    for (int k = 0; k < c.nRecent; ++k)
    {
        const uint32_t ev = c.recent[k];
        if (ev >= kEvents.size())
            continue;
        const kessler::Event &e = kEvents[ev];
        const glm::dvec3 B(e.pos[0], e.pos[1], e.pos[2]);
        const glm::dvec3 d = glm::normalize(eciToEcef(B, e.t));
        const bool user = e.origin == kessler::ORIGIN_USER;
        const bool hot = trSelEvent == k || (trHoverMarker >= 0 && trHoverMarker < (int)trMarkers.size() &&
                                             trMarkers[(size_t)trHoverMarker].rank == k + 1);
        CLAY(CLAY_IDI("TrEv", (uint32_t)k), {.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0)},
                                                        .padding = {8, 8, 5, 5},
                                                        .childGap = 8,
                                                        .childAlignment = {.y = CLAY_ALIGN_Y_CENTER},
                                                        .layoutDirection = CLAY_LEFT_TO_RIGHT},
                                             .backgroundColor = hot ? Pal::rowHighlight : Pal::cardBg,
                                             .cornerRadius = CLAY_CORNER_RADIUS(4),
                                             .border = {.color = Pal::cardBorder, .width = CLAY_BORDER_ALL(1)}})
        {
            if (Clay_Hovered() && inp.lmbPressed)
                trSelEvent = k;
            CLAY(CLAY_IDI("TrEvN", (uint32_t)k), {.layout = {.sizing = {CLAY_SIZING_FIXED((float)fs(18)), CLAY_SIZING_FIT(0)}}})
            {
                text(msStr("%d", k + 1), user ? Pal::trkMarkerUser : Pal::trkMarker, 14);
            }
            CLAY(CLAY_IDI("TrEvT", (uint32_t)k), {.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0)},
                                                             .childGap = 2,
                                                             .layoutDirection = CLAY_TOP_TO_BOTTOM}})
            {
                const char *kind = e.kind == kessler::EV_COLLISION ? (e.catastrophic ? "Catastrophic collision" : "Collision")
                                   : e.kind == kessler::EV_EXPLOSION ? "Explosion"
                                   : e.kind == kessler::EV_ASAT      ? "ASAT strike"
                                                                     : "Breakup";
                text(msStr("%s%s: %s", kind, user ? " (yours)" : "", kesslerEventTitle(e).c_str()), Pal::textPrimary, 12, true);
                char u[48], ago[40];
                fmtUtc(e.t, u, sizeof(u));
                fmtAgo(now - e.t, ago, sizeof(ago));
                const double lat = glm::degrees(std::asin(std::clamp(d.z, -1.0, 1.0))), lon = glm::degrees(std::atan2(d.y, d.x));
                text(msStr("%s (%s), %.0f km up over %.0f %s %.0f %s%s", u, ago, (glm::length(B) - kR) / 1000.0, std::abs(lat),
                           lat >= 0.0 ? "N" : "S", std::abs(lon), lon >= 0.0 ? "E" : "W",
                           e.kind == kessler::EV_COLLISION ? msStr(", %.1f km/s", e.vRel / 1000.0) : ""),
                     Pal::textDim, 11, true);
            }
            if (uiButton(inp, ui, "TrWatch", k, "Watch", "The debris just after the breakup, from 80 km beside its orbit"))
                watch = k;
        }
    }
    if (watch >= 0 && watch < c.nRecent)
    {
        std::string msg;
        kesslerWatchEvent(c.recent[watch], msg);
        msStatus = msg;
        trSelEvent = watch;
    }
}
