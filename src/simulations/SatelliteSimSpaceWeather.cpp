// SatelliteSimSpaceWeather.cpp — the Sky events window's SPACE WEATHER tab: the auroral oval of both hemispheres as a CPU
// raster (the Kessler tracker's pattern: an RGBA8 image in one of the UI's external-image slots, re-made when the time
// moves, drawn with UITextBlock marks over it), and the Watch that puts the player under it (2026-10-30, issue
// 20261029_205641; CLAUDE.md "Sky events window").
//
// The raster uses the SAME functions the renderer does: this frame's spaceWeather_ (spaceWeatherAt, the Kp and the
// substorm) and auroraGpu_ (auroraGpuParams) through auroraOvalWeightCpu, the CPU mirror of shaders/include/aurora_oval.glsl
// without the noise. Each panel is a polar plot from above one magnetic pole (colatitude 0..55 deg, magnetic noon at the
// top, midnight at the bottom, dawn on the right in the north), land and sea shaded by the Sun so the night side reads dark.
#include "../FrameProfile.h"
#include "SatelliteSim.h"
#include "SpaceWeather.h"
#include "SatPhotometry.h"
#include "../UIRenderer.h"
#include "../VulkanContext.h"
#include "../Log.h"
#include "clay.h"
#include "UIPalette.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>

std::string fmtUtcReadable(double tJ2000); // SatelliteSimLaunches.cpp

namespace
{
double wallS() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
Clay_String cs(const char *s) { return Clay_String{false, (int32_t)strlen(s), s}; }
constexpr double kPiD = 3.14159265358979323846;
constexpr double kMaxColatDeg = 55.0;  // the panel's edge
glm::dvec3 eciToEcefV(const glm::dvec3 &v, double t)
{
    const double th = earthRotationAngle(t), c = std::cos(th), s = std::sin(th);
    return glm::dvec3(c * v.x + s * v.y, -s * v.x + c * v.y, v.z);
}
} // namespace

// ── The image ─────────────────────────────────────────────────────────────────────────────────────────────────────
bool SatelliteSim::spaceWeatherEnsureImage(UIRenderer &ui)
{
    if (swImg != VK_NULL_HANDLE)
        return swUiId != 0;
    if (!ctx_ || swFailed)
        return false;
    VulkanContext &ctx = *ctx_;
    ctx.createImage(kSwW, kSwH, VK_FORMAT_R8G8B8A8_SRGB, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, swImg, swImgMem);
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = swImg;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = VK_FORMAT_R8G8B8A8_SRGB;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCreateImageView(ctx.device, &vi, nullptr, &swView);
    VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    si.magFilter = si.minFilter = VK_FILTER_LINEAR;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    vkCreateSampler(ctx.device, &si, nullptr, &swSampler);
    swStageMapped = std::calloc((size_t)kSwW * kSwH * 4, 1); // (the CPU copy; each upload takes this frame's staging)
    VkCommandBuffer cmd = ctx.beginOneTimeCommands();
    ctx.imageBarrier(cmd, swImg, 0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                     VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    const VkClearColorValue clear{{0.01f, 0.015f, 0.03f, 1.0f}};
    const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(cmd, swImg, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear, 1, &range);
    ctx.imageBarrier(cmd, swImg, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    ctx.endOneTimeCommands(cmd);
    swUiId = ui.registerImage(ctx.device, swView, swSampler);
    if (swUiId == 0)
    {
        swFailed = true;
        Log::line("space weather: no free UI image slot for the auroral oval");
    }
    return swUiId != 0;
}

void SatelliteSim::spaceWeatherRecordUpload(VkCommandBuffer cmd)
{
    if (!swUploadPending || swImg == VK_NULL_HANDLE || !ctx_)
        return;
    swUploadPending = false;
    VulkanContext &c = *ctx_;
    c.imageBarrier(cmd, swImg, VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    const VkDeviceSize bytes = (VkDeviceSize)kSwW * kSwH * 4;
    const VulkanContext::StagingAlloc st = c.stagingAlloc(bytes);
    std::memcpy(st.ptr, swStageMapped, (size_t)bytes);
    VkBufferImageCopy rg{};
    rg.bufferOffset = st.off;
    rg.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    rg.imageExtent = {(uint32_t)kSwW, (uint32_t)kSwH, 1};
    vkCmdCopyBufferToImage(cmd, st.buf, swImg, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &rg);
    c.imageBarrier(cmd, swImg, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
}

void SatelliteSim::spaceWeatherDestroy(VkDevice device)
{
    std::free(swStageMapped);
    swStageMapped = nullptr;
    if (swSampler) vkDestroySampler(device, swSampler, nullptr);
    if (swView) vkDestroyImageView(device, swView, nullptr);
    if (swImg) vkDestroyImage(device, swImg, nullptr);
    if (swImgMem) vkFreeMemory(device, swImgMem, nullptr);
    swSampler = VK_NULL_HANDLE;
    swView = VK_NULL_HANDLE;
    swImg = VK_NULL_HANDLE;
    swImgMem = VK_NULL_HANDLE;
}

// ── The raster ────────────────────────────────────────────────────────────────────────────────────────────────────
// Both hemispheres at sim time t from this frame's oval (auroraGpu_ / spaceWeather_, which are t's when t is now).
void SatelliteSim::spaceWeatherRaster(double t)
{
    if (!swStageMapped)
        return;
    FPROF_SCOPE("spaceWeatherRaster");
    const auto t0 = std::chrono::steady_clock::now();
    const AuroraGpu &g = auroraGpu_;
    const glm::dvec3 pole = geomagPoleEcef();
    const glm::dvec3 mid = glm::normalize(glm::dvec3(g.midnight.x, g.midnight.y, g.midnight.z));
    const glm::dvec3 sunE = glm::normalize(eciToEcefV(glm::dvec3(sunDirECI), t));
    uint8_t *px = static_cast<uint8_t *>(swStageMapped);
    const double R = 94.0;
    for (int p = 0; p < 2; ++p)
    {
        const glm::dvec3 P = p == 0 ? pole : -pole;
        const glm::dvec3 east = glm::cross(P, mid);
        const double sgn = p == 0 ? 1.0 : -1.0; // (the south plot is seen from outside: east runs the other way)
        const double cx = p * kSwPanel + 0.5 * kSwPanel, cy = 0.5 * kSwH;
        for (int j = 0; j < kSwH; ++j)
            for (int i = 0; i < kSwPanel; ++i)
            {
                uint8_t *o = &px[((size_t)j * kSwW + (size_t)(p * kSwPanel + i)) * 4];
                const double dx = i + 0.5 - 0.5 * kSwPanel, dy = j + 0.5 - cy;
                const double rr = std::hypot(dx, dy);
                glm::vec3 col(3.0f, 4.0f, 9.0f);
                if (rr <= R + 1.5)
                {
                    const double th = glm::radians(std::min(rr, R) / R * kMaxColatDeg);
                    const double ph = std::atan2(sgn * dx, dy); // 0 = midnight (down), +90 = east (right in the north)
                    const glm::dvec3 d = std::cos(th) * P + std::sin(th) * (std::cos(ph) * mid + std::sin(ph) * east);
                    const double lat = std::asin(std::clamp(d.z, -1.0, 1.0)), lon = std::atan2(d.y, d.x);
                    const bool sea = oceanAt(lat, lon);
                    const double lit = glm::dot(d, sunE);
                    const float f = (float)glm::smoothstep(-0.12, 0.12, lit);
                    glm::vec3 base = sea ? glm::vec3(18.0f, 38.0f, 66.0f) : glm::vec3(56.0f, 62.0f, 50.0f);
                    base *= 0.22f + 0.78f * f;
                    // magnetic latitude rings every 10 deg of colatitude, and the 06 / 12 / 18 / 24 MLT spokes
                    const double ringD = std::abs(std::remainder(rr / R * kMaxColatDeg, 10.0)) * (R / kMaxColatDeg);
                    if (ringD < 0.55) base += glm::vec3(22.0f);
                    const double spoke = std::min(std::abs(dx), std::abs(dy));
                    if (spoke < 0.5) base += glm::vec3(14.0f);
                    // the oval: the CPU mirror of the shader's band, green with a red-magenta heart where it is brightest
                    const float w = std::clamp(auroraOvalWeightCpu(g, d), 0.0f, 1.0f);
                    base = base * (1.0f - 0.55f * w) + glm::vec3(30.0f, 236.0f, 108.0f) * w + glm::vec3(150.0f, 0.0f, 60.0f) * std::max(0.0f, w - 0.7f) / 0.3f * w;
                    if (rr > R - 0.8) base = glm::mix(base, glm::vec3(120.0f, 130.0f, 150.0f), 0.8f);
                    col = glm::clamp(base, glm::vec3(0.0f), glm::vec3(255.0f));
                }
                o[0] = (uint8_t)col.r;
                o[1] = (uint8_t)col.g;
                o[2] = (uint8_t)col.b;
                o[3] = 255;
            }
        (void)cx;
    }
    swRasterT = t;
    swRasterWall = wallS();
    swUploadPending = true;
    swRasterMs = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

// ── The tab ───────────────────────────────────────────────────────────────────────────────────────────────────────
void SatelliteSim::buildSkySpaceWeatherTab(const UIInput &inp, UIRenderer &ui, float bodyW)
{
    const double now = (double)simDayJ2000 * 86400.0 + simSecInDay;
    auto text = [&](const char *s, Clay_Color c, int size) {
        CLAY_TEXT(cs(s), CLAY_TEXT_CONFIG({.textColor = c, .fontSize = fs(size), .wrapMode = CLAY_TEXT_WRAP_NONE}));
    };
    const SpaceWeather &sw = spaceWeather_;
    const AuroraOvalBounds b = auroraOvalBounds(sw.kp);

    // ── The oval (both hemispheres, from above the magnetic poles) ──
    uiSection("SwSecOval", 0, tx(Txt::SecAuroralOval));
    {
        const bool haveImg = spaceWeatherEnsureImage(ui);
        // Re-made when the time moved a minute, at most twice a second; the first time at once.
        if (haveImg && (swRasterT < -1e299 || (std::abs(now - swRasterT) > 60.0 && wallS() - swRasterWall > 0.5)))
            spaceWeatherRaster(now);
        const float W = std::floor(std::max(160.0f, bodyW - 8.0f)), H = std::floor(W * 0.5f), k = W / (float)kSwW;
        swQuads.clear();
        swRuns.clear();
        swRunText.clear();
        auto label = [&](float x, float y, const char *s) {
            swRunText.emplace_back(s);
            swRuns.push_back({swRunText.back().c_str(), (int)swRunText.back().size(), x, y});
        };
        const float fsz = (float)fs(9);
        for (int p = 0; p < 2; ++p)
        {
            const float cx = (p * kSwPanel + 0.5f * kSwPanel) * k, cy = 0.5f * kSwH * k, Rk = 94.0f * k;
            label(cx - fsz * 0.55f, cy - Rk - fsz * 1.1f + fsz * 0.3f, "12");
            label(cx - fsz * 0.55f, cy + Rk - fsz * 0.6f, "00");
            label(cx - Rk + fsz * 0.3f, cy - fsz * 0.5f, p == 0 ? "18" : "06"); // (inside the rim: the panels are side by side)
            label(cx + Rk - fsz * 1.4f, cy - fsz * 0.5f, p == 0 ? "06" : "18");
            label(p * kSwPanel * k + 3.0f, 2.0f, p == 0 ? tx(Txt::ValNorth) : tx(Txt::ValSouth));
        }
        // You: the observer's place on its hemisphere's plot (cyan dot).
        {
            const glm::dvec3 pole = geomagPoleEcef(), mid = glm::normalize(glm::dvec3(auroraGpu_.midnight.x, auroraGpu_.midnight.y, auroraGpu_.midnight.z));
            const glm::dvec3 u = glm::normalize(glm::dvec3(obsDir));
            const int p = glm::dot(u, pole) >= 0.0 ? 0 : 1;
            const glm::dvec3 P = p == 0 ? pole : -pole, east = glm::cross(P, mid);
            const double th = std::acos(std::clamp(glm::dot(u, P), -1.0, 1.0));
            if (glm::degrees(th) < kMaxColatDeg)
            {
                const double ph = std::atan2(glm::dot(u, east), glm::dot(u, mid));
                const double sgn = p == 0 ? 1.0 : -1.0, r = th / glm::radians(kMaxColatDeg) * 94.0;
                const float x = (float)((p * kSwPanel + 0.5 * kSwPanel + sgn * r * std::sin(ph)) * k), y = (float)((0.5 * kSwH + r * std::cos(ph)) * k);
                swQuads.push_back({x - 3.0f, y - 3.0f, 6.0f, 6.0f, glm::vec4(0.35f, 0.86f, 1.0f, 1.0f)});
            }
        }
        swMarks.runs = swRuns.data();
        swMarks.runCount = (int)swRuns.size();
        swMarks.quads = swQuads.data();
        swMarks.quadCount = (int)swQuads.size();
        swMarks.fontSize = fsz;
        swMarks.color = glm::vec4(0.8f, 0.86f, 0.95f, 1.0f);
        swEl.imageId = swUiId;
        CLAY(CLAY_ID("SwOvalBack"), {.layout = {.sizing = {CLAY_SIZING_FIXED(W), CLAY_SIZING_FIXED(H)}},
                                     .backgroundColor = Pal::insetBg,
                                     .cornerRadius = CLAY_CORNER_RADIUS(4)})
        {
            if (haveImg)
            {
                CLAY(CLAY_ID("SwOvalImg"), {.layout = {.sizing = {CLAY_SIZING_FIXED(W), CLAY_SIZING_FIXED(H)}}, .custom = {.customData = &swEl}})
                {
                    CLAY(CLAY_ID("SwOvalMarks"), {.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0)}}, .custom = {.customData = &swMarks}}) {}
                }
            }
        }
    }

    // ── Now ──
    uiSection("SwSecNow", 1, tx(Txt::SecSpaceWeatherNow));
    {
        const int G = geomagStormScale(sw.kp);
        char kpS[16], gS[16], edS[24], cyS[16];
        std::snprintf(kpS, sizeof(kpS), "%.1f", sw.kp);
        std::snprintf(gS, sizeof(gS), G > 0 ? "G%d" : "-", G);
        std::snprintf(edS, sizeof(edS), "%.0f deg", 90.0 - b.eqMid);
        std::snprintf(cyS, sizeof(cyS), "%.0f%%", 100.0 * sw.cycle);
        const Clay_Color kc = sw.kp >= 7.0 ? Pal::furyText : sw.kp >= 5.0 ? Pal::textWarn : Pal::textBright;
        CLAY(CLAY_ID("SwTiles"), {.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0)}, .childGap = 6, .layoutDirection = CLAY_LEFT_TO_RIGHT}})
        {
            CLAY(CLAY_ID("SwTile0"), {.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0)}}}) { uiStatTile("SwTileK", 0, tx(Txt::LblKpIndex), skStr("%s", kpS), kc); }
            CLAY(CLAY_ID("SwTile1"), {.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0)}}}) { uiStatTile("SwTileK", 1, tx(Txt::LblStormScale), skStr("%s", gS), kc); }
            CLAY(CLAY_ID("SwTile2"), {.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0)}}}) { uiStatTile("SwTileK", 2, tx(Txt::LblOvalEdge), skStr("%s", edS), Pal::textBright); }
            CLAY(CLAY_ID("SwTile3"), {.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0)}}}) { uiStatTile("SwTileK", 3, tx(Txt::LblSolarCycle), skStr("%s", cyS), Pal::textBright); }
        }
        char buf[160];
        std::snprintf(buf, sizeof(buf), tx(Txt::FmtSwDrivers), sw.kpQuiet, sw.hss, sw.cme);
        uiKV("SwKV", 0, tx(Txt::LblSwDrivers), skStr("%s", buf));
        if (sw.cmePeakKp > 0.0)
        {
            std::snprintf(buf, sizeof(buf), tx(Txt::FmtSwCmeStorm), sw.cmePeakKp, sw.cmeHoursSince);
            uiKV("SwKV", 1, tx(Txt::LblSwStorm), skStr("%s", buf));
        }
        if (sw.subMinutes >= 0.0)
        {
            std::snprintf(buf, sizeof(buf), tx(Txt::FmtSwSubstorm), sw.subMinutes, sw.subPoleDeg);
            uiKV("SwKV", 2, tx(Txt::LblSwSubstorm), skStr("%s", buf));
        }
        const bool dis = tierNeedReason(2u) != nullptr; // (kNeedAurora)
        if (uiButton(inp, ui, "SwWatchNow", 0, tx(Txt::BtnWatchTheAurora), dis ? tierNeedReason(2u) : tx(Txt::TipWatchAuroraNow), false, !dis, true))
            skPendingSpecial = 100; // acted on after the layout
    }

    // ── The next storms (CME-driven, the same hashes as the index; the Sun's Fury among them) ──
    uiSection("SwSecNext", 2, tx(Txt::SecNextStorms));
    int shown = 0;
    for (const sky::Event &e : skFound[sky::kCme])
    {
        if (e.t1 <= now) continue;
        if (shown >= 5) break;
        const bool fury = e.a == 1;
        const double dt = e.t0 - now;
        const char *when = now >= e.t0 ? tx(Txt::TxtHappeningNow)
                           : dt > 2 * 86400.0 ? skStr(tx(Txt::FmtInDH2), (long long)(dt / 86400.0), (long long)std::fmod(dt / 3600.0, 24.0))
                                              : skStr(tx(Txt::FmtInHMin), (long long)(dt / 3600.0), (long long)std::fmod(dt / 60.0, 60.0));
        SkRowSpec rs;
        rs.key = "SwStormBtn";
        rs.section = 4;
        rs.idx = (uint32_t)shown;
        rs.icon = 45;
        rs.accent = fury ? &Pal::furyBorder : &Pal::tagTwilight;
        rs.special = fury;
        rs.head = skStr("%s", fury ? tx(Txt::TxtTheSunsFuryCme) : skStr(tx(Txt::FmtGeoStorm), geomagStormScale(e.sepDeg), e.sepDeg));
        rs.headCol = fury ? &Pal::furyText : &Pal::textPrimary;
        std::string u = fmtUtcReadable(e.t);
        if (u.size() > 19) u.erase(16, 3);
        rs.status = "";
        rs.d1 = skStr("%s  -  %s", u.c_str(), when);
        rs.btnMask = 4;
        rs.watchOk = tierNeedReason(2u) == nullptr;
        rs.tip = tx(Txt::TipWatchAuroraStorm);
        if (skyEventRow(inp, ui, rs) & 4)
            skPendingSpecial = 200 + (int)(&e - &skFound[sky::kCme][0]); // (a storm row's Watch: acted on after the layout)
        ++shown;
    }
    if (shown == 0)
        text(skScanKind > sky::kCme ? tx(Txt::TxtNoneInTheNext2Years) : tx(Txt::ValSearching), Pal::textDim, 11);
}

// ── Watch: the best dark place under the oval at t ─────────────────────────────────────────────────────────────────
bool SatelliteSim::spaceWeatherWatch(double t, std::string &msg)
{
    FPROF_SCOPE("spaceWeatherWatch");
    const double nowT = (double)simDayJ2000 * 86400.0 + simSecInDay;
    const bool atNow = std::abs(t - nowT) < 1.0;
    // The activity at t (a pure function of sim time), the oval it implies.
    const SpaceWeather sw = spaceWeatherAt(t, auroraStormRate, auroraActivityAuto >= 0.5f ? -1.0 : (double)auroraKpManual);
    const glm::dvec3 sunE = glm::normalize(eciToEcefV(sunDirEciAt(t), t));
    const AuroraGpu g = auroraGpuParams(sw, sunE);
    const glm::dvec3 pole = geomagPoleEcef();
    struct Pick
    {
        double sc = -1e30;
        glm::dvec3 u{0.0, 0.0, 1.0};
    };
    std::vector<Pick> cands;
    for (double sunMax : {-12.0, -6.0, 90.0})
    {
        cands.clear();
        for (double la = -78.0; la <= 78.0; la += 2.0)
        {
            const double cl = std::cos(glm::radians(la)), sl = std::sin(glm::radians(la));
            for (double lo = -180.0; lo < 180.0; lo += 3.0)
            {
                const glm::dvec3 u(cl * std::cos(glm::radians(lo)), cl * std::sin(glm::radians(lo)), sl);
                const double sunAlt = glm::degrees(std::asin(std::clamp(glm::dot(sunE, u), -1.0, 1.0)));
                if (sunAlt > sunMax) continue;
                const float w = auroraOvalWeightCpu(g, u);
                if (w < 0.05f) continue;
                const float city = nightLumCpu((float)la, (float)lo);
                const double sc = w - 0.02 * std::max(0.0f, city - 16.0f) + 0.002 * std::min(-sunAlt, 18.0);
                cands.push_back({sc, u});
            }
        }
        if (!cands.empty()) break;
    }
    if (cands.empty())
    {
        msg = tx(Txt::ValSwNoDarkPlace);
        return false;
    }
    std::sort(cands.begin(), cands.end(), [](const Pick &a, const Pick &b) { return a.sc > b.sc; });
    if (cands.size() > 40) cands.resize(40);
    if (!atNow) skyJumpTo(t); // (first: the cloud map's drift is that moment's)
    else skyJumpTo(nowT);
    glm::dvec3 site = cands[0].u;
    double bestSc = -1e30;
    for (const Pick &p : cands)
    {
        const double la = glm::degrees(std::asin(std::clamp(p.u.z, -1.0, 1.0))), lo = glm::degrees(std::atan2(p.u.y, p.u.x));
        double sc = p.sc - 0.5 * cloudCoverAtEcef(p.u);
        if (oceanFraction(glm::radians(la), glm::radians(lo), 20000.0, 1) < 0.5f) sc += 0.06; // ground under the observer
        if (sc > bestSc) { bestSc = sc; site = p.u; }
    }
    skyPlaceAt(site, t);
    selectedSatIndex = -1;
    selectedPlanetIndex = -1;
    skAimA = skAimB = -1;
    // Toward the magnetic pole of this hemisphere, 28 deg up, wide: the oval stands over the poleward horizon.
    const glm::dvec3 P = site.z >= 0.0 ? pole : -pole;
    const glm::dvec3 up = site, east = glm::normalize(glm::cross(glm::dvec3(0, 0, 1), up)), north = glm::cross(up, east);
    const double az = glm::degrees(std::atan2(glm::dot(P, east), glm::dot(P, north)));
    aimCameraAzEl((float)az, 28.0f);
    camera.fovYDeg = 90.0f;
    char b[260];
    std::snprintf(b, sizeof(b), tx(Txt::FmtSwWatch), sw.kp, std::abs(obsLatDeg), obsLatDeg >= 0 ? "N" : "S", std::abs(obsLonDeg),
                  obsLonDeg >= 0 ? "E" : "W", 90.0 - auroraOvalBounds(sw.kp).eqMid,
                  -glm::degrees(std::asin(std::clamp((double)sunDirENU.z, -1.0, 1.0))));
    msg = b;
    return true;
}
