// SatelliteSimWeatherCyclones.cpp — the Weather window's named storms (2026-10-23, .plans/WEATHER_EVENTS_PLAN.md
// "Addendum: weather like missions").
//
// The tropical cyclones (Weather.h wx::Cyclone: one per basin, season and index, the whole hourly track and intensity
// fixed at genesis, Earth-fixed) seen like Missions: the Cyclones tab lists the active ones (category in the basin's
// term, wind, pressure, motion, age, over land), the selected basin's season (past, active, upcoming) and the next
// genesis in every basin; a map of the tracks (an equirect land / sea raster, re-made only when its box changes, with
// the tracks as overlay dots in Saffir-Simpson colours: history bright, forecast fainter — the whole track is known);
// the selected cyclone's milestones (genesis, named, hurricane / typhoon / cyclone strength, rapid intensification,
// peak, landfalls, dissipation) with Skip (the clock there at 1x, the camera on the storm), Go to (near, from orbit,
// in the eye, its landfall point) and Watch (its peak, or genesis while upcoming). The radar draws the tracks too
// (SatelliteSimWeatherWindow.cpp). Everything here runs only while the window is open; the lists are copies (the
// catalog's seasons may be rebuilt under a pointer), refreshed every 2 s or after a jump in sim time.
#include "../FrameProfile.h"
#include "SatelliteSim.h"
#include "../UIRenderer.h"
#include "../VulkanContext.h"
#include "../Log.h"
#include "clay.h"
#include "UIPalette.h"

#include <nlohmann/json.hpp>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>

namespace
{
constexpr double kRe = 6371000.0;
constexpr double kDayS = 86400.0;
double wallNow() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
Clay_String cs(const char *s) { return Clay_String{false, (int32_t)strlen(s), s}; }
glm::vec4 palV(const Clay_Color &c, float a) { return glm::vec4(c.r / 255.0f, c.g / 255.0f, c.b / 255.0f, a); }

// = Weather.cpp cycloneCategory: -1 depression, 0 tropical storm, 1-5 Saffir-Simpson (1-min wind, m/s).
int catOf(float v) { return v < 17.5f ? -1 : v < 33.0f ? 0 : v < 43.0f ? 1 : v < 50.0f ? 2 : v < 58.0f ? 3 : v < 70.0f ? 4 : 5; }
float pressureOf(float v)
{
    const double kt = std::max(0.0, (double)v * 1.944);
    return (float)(1010.0 - std::pow(kt / 6.7, 1.0 / 0.644));   // Atkinson-Holliday, as Catalog::cyclonesAt
}
// The NHC track maps' Saffir-Simpson colours (data colours, sRGB 0..255).
glm::vec3 ssColor(int cat)
{
    static const glm::vec3 k[7] = {{94, 186, 255}, {0, 250, 244}, {255, 255, 204}, {255, 231, 117},
                                   {255, 193, 64}, {255, 143, 32}, {255, 96, 96}};
    return k[std::clamp(cat + 1, 0, 6)];
}
Clay_Color ssClay(int cat)
{
    const glm::vec3 c = ssColor(cat);
    return Clay_Color{c.r, c.g, c.b, 255};
}
const char *catShort(int cat)
{
    static const char *k[7] = {"TD", "TS", "C1", "C2", "C3", "C4", "C5"};
    return k[std::clamp(cat + 1, 0, 6)];
}
const char *basinShort(int b)
{
    static const char *k[6] = {"N Atl", "E Pac", "W Pac", "N Ind", "SW Ind", "Aus/S Pac"};
    return (b >= 0 && b < 6) ? k[b] : "?";
}
const char *strengthTerm(int basin)
{
    return basin <= 1 ? "Hurricane strength" : basin == 2 ? "Typhoon strength" : basin == 3 ? "Very severe cyclonic storm"
                                                                                           : "Severe tropical cyclone";
}
const char *compass(double bearingDeg)
{
    static const char *k[16] = {"N", "NNE", "NE", "ENE", "E", "ESE", "SE", "SSE", "S", "SSW", "SW", "WSW", "W", "WNW", "NW", "NNW"};
    return k[(int)std::floor(std::fmod(bearingDeg + 360.0 + 11.25, 360.0) / 22.5) % 16];
}
void fmtRel(double dt, char *b, size_t n)   // "in 2d 04h" / "5h 10m ago"
{
    const double a = std::abs(dt);
    const long long m = (long long)std::floor(a / 60.0);
    char core[48];
    if (m >= 1440)
        snprintf(core, sizeof(core), "%lldd %02lldh", m / 1440, (m % 1440) / 60);
    else if (m >= 60)
        snprintf(core, sizeof(core), "%lldh %02lldm", m / 60, m % 60);
    else
        snprintf(core, sizeof(core), "%lld min", m);
    if (dt >= 0.0)
        snprintf(b, n, "in %s", core);
    else
        snprintf(b, n, "%s ago", core);
}
void fmtDate(double tJ2000, char *b, size_t n, bool withTime)
{
    const time_t u = (time_t)std::floor(tJ2000) + 946728000;
    const struct tm *g = gmtime(&u);
    static const char *mon[12] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    if (!g)
        snprintf(b, n, "--");
    else if (withTime)
        snprintf(b, n, "%s %02d %02d:%02d UTC", mon[g->tm_mon], g->tm_mday, g->tm_hour, g->tm_min);
    else
        snprintf(b, n, "%s %02d", mon[g->tm_mon], g->tm_mday);
}
glm::dvec3 tangentEast(const glm::dvec3 &p) { return glm::normalize(glm::cross(glm::dvec3(0, 0, 1), p) + glm::dvec3(1e-12, 0, 0)); }
double bearingTo(const glm::dvec3 &from, const glm::dvec3 &to)   // degrees from north, east positive
{
    const glm::dvec3 E = tangentEast(from), N = glm::cross(from, E);
    const glm::dvec3 d = to - from * glm::dot(to, from);
    return glm::degrees(std::atan2(glm::dot(d, E), glm::dot(d, N)));
}
double latOf(const glm::dvec3 &p) { return glm::degrees(std::asin(std::clamp(p.z, -1.0, 1.0))); }
double lonOf(const glm::dvec3 &p) { return glm::degrees(std::atan2(p.y, p.x)); }
// The cyclone's outer radius at a wind (= Catalog::cyclonesAt's outerR).
double outerRadiusM(const wx::Cyclone &c, float v)
{
    const double s = std::clamp((v - 12.0) / 18.0, 0.0, 1.0);
    return 1000.0 * (350.0 + 450.0 * c.size) * (0.4 + 0.6 * s * s * (3.0 - 2.0 * s));
}
} // namespace

// ── The lists ───────────────────────────────────────────────────────────────────────────────────────────────────
void SatelliteSim::weatherRefreshCyclones(bool force)
{
    if (!stormCatalog)
        return;
    const double now = (double)simDayJ2000 * kDayS + simSecInDay;
    const double wall = wallNow();
    if (!force && wxCycEnvKey == stormEnvKey && wall - wxCycWall < 2.0 && std::abs(now - wxCycSim) < 3600.0)
        return;
    FPROF_SCOPE("weatherRefreshCyclones");
    wxCycWall = wall;
    wxCycSim = now;
    wxCycEnvKey = stormEnvKey;
    wxCycActive.clear();
    for (const wx::CycloneState &S : stormCyclonesNow)
        if (S.c)
            wxCycActive.push_back(*S.c);
    const int y = (int)std::floor(now / (365.2422 * kDayS) + 2000.0);
    int b = wxCycBasin;
    if (b < 0)
        b = !wxCycActive.empty() ? wxCycActive.front().basin : 0;
    std::vector<const wx::Cyclone *> v;
    wxCycSeason.clear();
    for (int yy = y - 1; yy <= y + 1; ++yy)
    {
        stormCatalog->cycloneSeason(b, yy, v);
        for (const wx::Cyclone *c : v)
            if (c->t0 + c->life > now - 200.0 * kDayS && c->t0 < now + 200.0 * kDayS)
                wxCycSeason.push_back(*c);
    }
    std::sort(wxCycSeason.begin(), wxCycSeason.end(), [](const wx::Cyclone &a, const wx::Cyclone &c) { return a.t0 < c.t0; });
    // The next genesis in every basin (this year's season or the next).
    wxCycNext.clear();
    for (int bb = 0; bb < wx::Catalog::kCycloneBasins; ++bb)
    {
        const wx::Cyclone *best = nullptr;
        wx::Cyclone keep;
        for (int yy = y; yy <= y + 1 && !best; ++yy)
        {
            stormCatalog->cycloneSeason(bb, yy, v);
            for (const wx::Cyclone *c : v)
                if (c->t0 > now && (!best || c->t0 < best->t0))
                    best = c;
            if (best)
                keep = *best;
        }
        if (best)
            wxCycNext.push_back(keep);
    }
    std::sort(wxCycNext.begin(), wxCycNext.end(), [](const wx::Cyclone &a, const wx::Cyclone &c) { return a.t0 < c.t0; });
    if (wxSelCyclone == 0 && !wxCycActive.empty())
        wxSelCyclone = wxCycActive.front().id;
}

const wx::Cyclone *SatelliteSim::weatherCycloneById(uint64_t id) const
{
    for (const auto *lst : {&wxCycActive, &wxCycSeason, &wxCycNext})
        for (const wx::Cyclone &c : *lst)
            if (c.id == id)
                return &c;
    return nullptr;
}

void SatelliteSim::weatherCycloneMilestones(const wx::Cyclone &c, std::vector<WxMilestone> &out) const
{
    out.clear();
    const size_t n = c.vmax.size();
    out.push_back({c.t0, "Genesis (tropical depression)", 0});
    int named = -1, hur = -1, major = -1, ri = -1;
    for (size_t i = 0; i < n; ++i)
    {
        const float v = c.vmax[i];
        if (named < 0 && v >= 17.5f) named = (int)i;
        if (hur < 0 && v >= 33.0f) hur = (int)i;
        if (major < 0 && v >= 50.0f) major = (int)i;
        if (ri < 0 && i >= 24 && v - c.vmax[i - 24] >= 15.4f) ri = (int)i - 24;   // +30 kt in 24 h: its onset
    }
    char b[160];
    if (named >= 0)
    {
        snprintf(b, sizeof(b), "%s %s", c.name.empty() ? "Tropical storm" : "Named", c.name.c_str());
        out.push_back({c.t0 + named * wx::Cyclone::kStepS, b, 1});
    }
    if (hur >= 0)
        out.push_back({c.t0 + hur * wx::Cyclone::kStepS, strengthTerm(c.basin), 2});
    if (ri >= 0)
        out.push_back({c.t0 + ri * wx::Cyclone::kStepS, "Rapid intensification begins (+15 m/s in 24 h)", 3});
    if (major >= 0)
        out.push_back({c.t0 + major * wx::Cyclone::kStepS, "Category 3 (major)", 4});
    snprintf(b, sizeof(b), "Peak: %s, %.0f m/s (%.0f kt), %.0f hPa", wx::cycloneCategoryName(catOf(c.vPeak), c.basin), c.vPeak,
             c.vPeak * 1.944f, pressureOf(c.vPeak));
    out.push_back({c.tPeak, b, 5});
    int nLand = 0;
    for (size_t i = 1; i < c.overLand.size(); ++i)
        if (c.overLand[i] && !c.overLand[i - 1])
        {
            const glm::dvec3 p = c.track[std::min(i, c.track.size() - 1)];
            snprintf(b, sizeof(b), "Landfall%s at %.1f %s %.1f %s, %s", ++nLand > 1 ? " (again)" : "", std::abs(latOf(p)),
                     latOf(p) >= 0 ? "N" : "S", std::abs(lonOf(p)), lonOf(p) >= 0 ? "E" : "W", catShort(catOf(c.vmax[i])));
            out.push_back({c.t0 + i * wx::Cyclone::kStepS, b, 6});
        }
    out.push_back({c.t0 + c.life, "Dissipation", 7});
    std::stable_sort(out.begin(), out.end(), [](const WxMilestone &a, const WxMilestone &m) { return a.t < m.t; });
}

// ── Actions ─────────────────────────────────────────────────────────────────────────────────────────────────────
namespace
{
void setSimTime(SatelliteSim &, int64_t &day, double &sec, double t)
{
    const double d = std::floor(t / kDayS);
    day = (int64_t)d;
    sec = t - d * kDayS;
}
} // namespace

bool SatelliteSim::weatherGoToCyclone(uint64_t id, int mode, std::string &msg)
{
    const wx::Cyclone *cp = weatherCycloneById(id);
    if (!cp)
    {
        msg = "That cyclone is not listed";
        return false;
    }
    const wx::Cyclone c = *cp;   // (the lists may be refreshed below)
    const double t = (double)simDayJ2000 * kDayS + simSecInDay;
    if (mode != 3 && (t < c.t0 || t > c.t0 + c.life))
    {
        msg = (c.name.empty() ? std::string("It") : c.name) + (t < c.t0 ? " has not formed yet (Watch jumps to its genesis)"
                                                                       : " has dissipated (Watch replays its peak)");
        return false;
    }
    const glm::dvec3 e = c.earthAt(t);
    const float v = c.vmaxAt(t);
    glm::dvec3 standAt = e, lookAt = e;
    float alt = 1500.0f, el = 6.0f, fov = std::max(camera.fovYDeg, 60.0f);
    bool down = false;
    const char *what = "";
    if (mode == 0)
    {
        // As `storms go cyclone=`: 60 km behind its motion, 1.5 km up, looking at the centre.
        const double bm = glm::radians(bearingTo(e, c.earthAt(t + 1800.0))) + glm::pi<double>();
        const glm::dvec3 E = tangentEast(e), N = glm::cross(e, E);
        const double ang = 60000.0 / kRe;
        standAt = glm::normalize(e * std::cos(ang) + (E * std::sin(bm) + N * std::cos(bm)) * std::sin(ang));
        what = "60 km behind it";
    }
    else if (mode == 1)
    {
        alt = (float)std::clamp(1.6 * outerRadiusM(c, v), 700000.0, 2500000.0);
        down = true;
        fov = 60.0f;
        what = "over the eye from orbit";
    }
    else if (mode == 2)
    {
        alt = 2500.0f;
        el = 14.0f;
        // Look across the eye toward the eyewall, ahead of the motion.
        lookAt = c.earthAt(t + 3 * 3600.0);
        what = v >= 33.0f ? "in the eye" : "at the centre (no eye yet)";
    }
    else
    {
        // The next landfall (else the first): stand there on the coast and look at the storm now.
        int iL = -1;
        const double k = (t - c.t0) / wx::Cyclone::kStepS;
        for (size_t i = 1; i < c.overLand.size(); ++i)
            if (c.overLand[i] && !c.overLand[i - 1])
            {
                if (iL < 0) iL = (int)i;
                if ((double)i >= k) { iL = (int)i; break; }
            }
        if (iL < 0)
        {
            msg = (c.name.empty() ? std::string("It") : c.name) + " never makes landfall";
            return false;
        }
        standAt = c.track[(size_t)iL];
        lookAt = (t >= c.t0 && t <= c.t0 + c.life) ? e : c.track[(size_t)std::max(0, iL - 6)];
        alt = 150.0f;
        el = 8.0f;
        what = "at its landfall point";
    }
    if (followActive)
        stopFollow();
    stopTrack();
    setObserverLatLon(latOf(standAt), lonOf(standAt));
    obsHeightOffset = alt;
    camera.fovYDeg = fov;
    if (down)
        aimCameraAzEl(0.0f, -89.5f);
    else
        aimCameraAzEl((float)(glm::length(lookAt - standAt) * kRe < 500.0 ? bearingTo(e, c.earthAt(t + 3600.0))
                                                                          : bearingTo(standAt, lookAt)),
                      el);
    updatePositions(t);
    trailClearPending = true;
    skyTaaHistValid = false;
    cv2HistoryValid = false;
    wxSelCyclone = c.id;
    char b[200];
    snprintf(b, sizeof(b), "%s (%s): %s", c.name.empty() ? "Tropical depression" : c.name.c_str(),
             wx::cycloneCategoryName(catOf(v), c.basin), what);
    msg = b;
    return true;
}

bool SatelliteSim::weatherWatchCyclone(uint64_t id, std::string &msg)
{
    const wx::Cyclone *cp = weatherCycloneById(id);
    if (!cp)
    {
        msg = "That cyclone is not listed";
        return false;
    }
    const double now = (double)simDayJ2000 * kDayS + simSecInDay;
    const bool upcoming = now < cp->t0;
    const double t = upcoming ? cp->t0 + 3600.0 : cp->tPeak;
    setSimTime(*this, simDayJ2000, simSecInDay, t);
    timePaused = false;
    timeScaleIdx = 0;
    timeDir = 1.0f;
    std::string m;
    const bool ok = weatherGoToCyclone(id, 1, m);
    msg = std::string(upcoming ? "Its genesis: " : "Its peak: ") + m;
    return ok;
}

bool SatelliteSim::weatherCycloneMilestone(uint64_t id, int index, std::string &msg)
{
    const wx::Cyclone *cp = weatherCycloneById(id);
    if (!cp)
    {
        msg = "That cyclone is not listed";
        return false;
    }
    std::vector<WxMilestone> ms;
    weatherCycloneMilestones(*cp, ms);
    if (index < 0 || index >= (int)ms.size())
    {
        msg = "No such milestone";
        return false;
    }
    const WxMilestone m = ms[(size_t)index];
    const bool land = m.kind == 6;
    // A landfall from the coast 2 h before it; dissipation 30 min before (it is still there); the rest at the moment.
    const double t = land ? m.t - 2.0 * 3600.0 : m.kind == 7 ? m.t - 1800.0 : std::max(m.t, cp->t0 + 60.0);
    setSimTime(*this, simDayJ2000, simSecInDay, t);
    timePaused = false;
    timeScaleIdx = 0;
    timeDir = 1.0f;
    std::string g;
    bool ok;
    if (land)
    {
        // Stand at THIS landfall's point (the track there), looking at the storm 2 h out.
        const wx::Cyclone c = *cp;
        const glm::dvec3 at = c.earthAt(m.t), now = c.earthAt(t);
        if (followActive) stopFollow();
        stopTrack();
        setObserverLatLon(latOf(at), lonOf(at));
        obsHeightOffset = 150.0f;
        camera.fovYDeg = std::max(camera.fovYDeg, 70.0f);
        aimCameraAzEl((float)bearingTo(at, now), 8.0f);
        updatePositions(t);
        trailClearPending = true;
        skyTaaHistValid = false;
        cv2HistoryValid = false;
        wxSelCyclone = c.id;
        g = "on the coast, 2 h before";
        ok = true;
    }
    else
        ok = weatherGoToCyclone(id, 1, g);
    msg = m.label + ": " + g;
    return ok;
}

// ── The map ─────────────────────────────────────────────────────────────────────────────────────────────────────
bool SatelliteSim::weatherMapEnsureImage(UIRenderer &ui)
{
    if (wxMapImg != VK_NULL_HANDLE)
        return wxMapUiId != 0;
    if (!ctx_ || wxMapFailed)
        return false;
    VulkanContext &ctx = *ctx_;
    ctx.createImage(kWxMapW, kWxMapH, VK_FORMAT_R8G8B8A8_SRGB, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                    wxMapImg, wxMapImgMem);
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = wxMapImg;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = VK_FORMAT_R8G8B8A8_SRGB;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCreateImageView(ctx.device, &vi, nullptr, &wxMapView);
    VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    si.magFilter = si.minFilter = VK_FILTER_LINEAR;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    vkCreateSampler(ctx.device, &si, nullptr, &wxMapSampler);
    const VkDeviceSize bytes = (VkDeviceSize)kWxMapW * kWxMapH * 4;
    ctx.createBuffer(bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                     wxMapStage, wxMapStageMem);
    vkMapMemory(ctx.device, wxMapStageMem, 0, bytes, 0, &wxMapStageMapped);
    std::memset(wxMapStageMapped, 0, (size_t)bytes);
    VkCommandBuffer cmd = ctx.beginOneTimeCommands();
    ctx.imageBarrier(cmd, wxMapImg, 0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                     VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    const VkClearColorValue clear{{0.02f, 0.03f, 0.05f, 1.0f}};
    const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(cmd, wxMapImg, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear, 1, &range);
    ctx.imageBarrier(cmd, wxMapImg, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    ctx.endOneTimeCommands(cmd);
    wxMapUiId = ui.registerImage(ctx.device, wxMapView, wxMapSampler);
    if (wxMapUiId == 0)
    {
        wxMapFailed = true;
        Log::line("weather: no free UI image slot for the cyclone map");
    }
    return wxMapUiId != 0;
}

// Land / sea in the box (equirect), the coasts, a graticule. Only when the box changed (~2-5 ms).
void SatelliteSim::weatherMapRaster()
{
    if (!wxMapStageMapped)
        return;
    FPROF_SCOPE("weatherMapRaster");
    const auto t0 = std::chrono::steady_clock::now();
    const double lon0 = wxMapBox[0], lat0 = wxMapBox[1], lon1 = wxMapBox[2], lat1 = wxMapBox[3];
    const double grid = (lon1 - lon0) > 120.0 ? 30.0 : (lon1 - lon0) > 50.0 ? 10.0 : 5.0;
    static std::vector<uint8_t> sea;
    sea.assign((size_t)kWxMapW * kWxMapH, 0);
    for (int j = 0; j < kWxMapH; ++j)
        for (int i = 0; i < kWxMapW; ++i)
        {
            const double lon = lon0 + (i + 0.5) / kWxMapW * (lon1 - lon0), lat = lat1 - (j + 0.5) / kWxMapH * (lat1 - lat0);
            sea[(size_t)j * kWxMapW + i] = oceanAt(glm::radians(lat), glm::radians(std::remainder(lon, 360.0))) ? 1 : 0;
        }
    uint8_t *px = static_cast<uint8_t *>(wxMapStageMapped);
    const double dLon = (lon1 - lon0) / kWxMapW, dLat = (lat1 - lat0) / kWxMapH;
    for (int j = 0; j < kWxMapH; ++j)
        for (int i = 0; i < kWxMapW; ++i)
        {
            const size_t k = (size_t)j * kWxMapW + i;
            const bool s = sea[k] != 0;
            glm::vec3 c = s ? glm::vec3(14, 26, 44) : glm::vec3(40, 44, 36);
            if ((i > 0 && sea[k - 1] != sea[k]) || (j > 0 && sea[k - kWxMapW] != sea[k]))
                c = glm::vec3(95, 105, 115);
            const double lon = lon0 + (i + 0.5) * dLon, lat = lat1 - (j + 0.5) * dLat;
            const double gx = std::abs(std::remainder(lon, grid)) / dLon, gy = std::abs(std::remainder(lat, grid)) / dLat;
            if (gx < 0.5 || gy < 0.5)
                c = glm::mix(c, glm::vec3(70, 80, 95), std::abs(lat) < 0.5 * dLat ? 0.6f : 0.3f);
            uint8_t *o = &px[k * 4];
            o[0] = (uint8_t)c.r;
            o[1] = (uint8_t)c.g;
            o[2] = (uint8_t)c.b;
            o[3] = 255;
        }
    std::memcpy(wxMapRastered, wxMapBox, sizeof(wxMapBox));
    wxMapUploadPending = true;
    wxMapCpuMs = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

// ── The tab ─────────────────────────────────────────────────────────────────────────────────────────────────────
void SatelliteSim::buildWeatherCyclonesTab(const UIInput &inp, UIRenderer &ui, float bodyW)
{
    const double now = (double)simDayJ2000 * kDayS + simSecInDay;
    auto text = [&](const char *s, Clay_Color c, int size, bool wrap = false) {
        CLAY_TEXT(cs(s), CLAY_TEXT_CONFIG({.textColor = c, .fontSize = fs(size),
                                           .wrapMode = wrap ? CLAY_TEXT_WRAP_WORDS : CLAY_TEXT_WRAP_NONE}));
    };
    auto tag = [&](uint32_t id, const char *label, Clay_Color bg, Clay_Color fg) {
        CLAY(CLAY_IDI("WxCTag", id), {.layout = {.sizing = {CLAY_SIZING_FIT(0), CLAY_SIZING_FIT(0)}, .padding = {5, 5, 1, 1}},
                                      .backgroundColor = bg,
                                      .cornerRadius = CLAY_CORNER_RADIUS(3)})
        {
            text(label, fg, 10);
        }
    };
    if (!stormObjects || stormCyclones <= 0.0f)
    {
        text("Tropical cyclones are off (Settings > Weather: Storm objects, Tropical cyclones).", Pal::textDim, 11, true);
        return;
    }
    const wx::Cyclone *sel = weatherCycloneById(wxSelCyclone);

    // ── The map ──
    {
        // Its box: the selected cyclone's track with a margin (2:1, at least 40 deg wide), else the world.
        double box[4] = {-180.0, -90.0, 180.0, 90.0};
        if (wxCycZoom && sel && !sel->track.empty())
        {
            const double lonRef = lonOf(sel->track.front());
            double a0 = 1e9, a1 = -1e9, b0 = 1e9, b1 = -1e9;
            for (const glm::dvec3 &p : sel->track)
            {
                const double lo = lonRef + std::remainder(lonOf(p) - lonRef, 360.0), la = latOf(p);
                a0 = std::min(a0, lo); a1 = std::max(a1, lo); b0 = std::min(b0, la); b1 = std::max(b1, la);
            }
            double cx = 0.5 * (a0 + a1), cy = 0.5 * (b0 + b1);
            double w = std::max({a1 - a0 + 16.0, 2.0 * (b1 - b0 + 12.0), 40.0});
            w = std::min(w, 360.0);
            cy = std::clamp(cy, -90.0 + 0.25 * w, 90.0 - 0.25 * w);
            // Snap to 2-degree steps so a refresh does not re-raster for nothing.
            cx = std::round(cx / 2.0) * 2.0;
            cy = std::round(cy / 2.0) * 2.0;
            w = std::ceil(w / 4.0) * 4.0;
            box[0] = cx - 0.5 * w; box[2] = cx + 0.5 * w; box[1] = cy - 0.25 * w; box[3] = cy + 0.25 * w;
        }
        std::memcpy(wxMapBox, box, sizeof(box));
        const bool haveImg = weatherMapEnsureImage(ui);
        if (haveImg && std::memcmp(wxMapBox, wxMapRastered, sizeof(wxMapBox)) != 0)
            weatherMapRaster();
        const float W = std::floor(std::max(160.0f, bodyW - 8.0f)), H = std::floor(W * 0.5f);
        auto toMap = [&](const glm::dvec3 &p, float &x, float &y) -> bool {
            const double lo = wxMapBox[0] + std::fmod(lonOf(p) - wxMapBox[0] + 720.0, 360.0), la = latOf(p);
            x = (float)((lo - wxMapBox[0]) / (wxMapBox[2] - wxMapBox[0])) * W;
            y = (float)((wxMapBox[3] - la) / (wxMapBox[3] - wxMapBox[1])) * H;
            return x >= 0.0f && x <= W && y >= 0.0f && y <= H;
        };
        wxMapQuads.clear();
        wxMapRuns.clear();
        wxMapRunText.clear();
        wxMapCycs.clear();
        auto label = [&](float x, float y, const char *s) {
            wxMapRunText.emplace_back(s);
            wxMapRuns.push_back({wxMapRunText.back().c_str(), (int)wxMapRunText.back().size(), x, y});
        };
        auto drawTrack = [&](const wx::Cyclone &c, bool hot) {
            const double span = wxMapBox[2] - wxMapBox[0];
            const size_t step = span > 200.0 ? 6 : span > 80.0 ? 3 : 2;
            const float r = hot ? 2.0f : 1.5f;
            for (size_t i = 0; i < c.track.size(); i += step)
            {
                float x, y;
                if (!toMap(c.track[i], x, y)) continue;
                const double ti = c.t0 + (double)i * wx::Cyclone::kStepS;
                const glm::vec3 col = ssColor(catOf(c.vmax[i])) / 255.0f;
                const bool past = ti <= now;
                const float a = (past ? 1.0f : 0.5f) * (hot ? 1.0f : 0.7f);
                const float rr = past ? r : r * 0.7f;
                wxMapQuads.push_back({x - rr, y - rr, 2.0f * rr, 2.0f * rr, glm::vec4(col, a)});
            }
            float x, y;
            if (now >= c.t0 && now <= c.t0 + c.life && toMap(c.earthAt(now), x, y))
            {
                const float rr = hot ? 6.0f : 5.0f;
                const glm::vec4 w(1.0f, 1.0f, 1.0f, 0.95f);
                wxMapQuads.push_back({x - rr, y - rr, 2.0f * rr, 1.5f, w});
                wxMapQuads.push_back({x - rr, y + rr - 1.5f, 2.0f * rr, 1.5f, w});
                wxMapQuads.push_back({x - rr, y - rr, 1.5f, 2.0f * rr, w});
                wxMapQuads.push_back({x + rr - 1.5f, y - rr, 1.5f, 2.0f * rr, w});
                label(x + rr + 2.0f, y - rr - 2.0f, c.name.empty() ? "TD" : c.name.c_str());
                wxMapCycs.push_back({x / W, y / H, c.id});
            }
            else if (hot && !c.track.empty() && toMap(c.track.front(), x, y))
                label(x + 3.0f, y - (float)fs(9), c.name.empty() ? "TD" : c.name.c_str());
        };
        for (const wx::Cyclone &c : wxCycActive)
            if (c.id != wxSelCyclone)
                drawTrack(c, false);
        if (sel)
            drawTrack(*sel, true);
        // You.
        {
            float x, y;
            if (toMap(glm::normalize(glm::dvec3(obsDir)), x, y))
                wxMapQuads.push_back({x - 2.5f, y - 2.5f, 5.0f, 5.0f, palV(Pal::trkYou, 1.0f)});
        }
        wxMapMarks.runs = wxMapRuns.data();
        wxMapMarks.runCount = (int)wxMapRuns.size();
        wxMapMarks.quads = wxMapQuads.data();
        wxMapMarks.quadCount = (int)wxMapQuads.size();
        wxMapMarks.fontSize = (float)fs(9);
        wxMapMarks.color = glm::vec4(0.95f, 0.96f, 0.98f, 1.0f);
        wxMapEl.imageId = wxMapUiId;
        // Hover / click: a cyclone's marker selects it.
        const Clay_ElementId mid = CLAY_ID("WxCycMap");
        const Clay_ElementData md = Clay_GetElementData(mid);
        wxMapHover = -1;
        if (md.found && md.boundingBox.width > 1.0f)
        {
            const float mx = (inp.mouseX - md.boundingBox.x) / md.boundingBox.width;
            const float my = (inp.mouseY - md.boundingBox.y) / md.boundingBox.height;
            if (mx >= 0.0f && mx <= 1.0f && my >= 0.0f && my <= 1.0f)
            {
                float best = 10.0f / md.boundingBox.width;
                for (size_t k = 0; k < wxMapCycs.size(); ++k)
                {
                    const float d = std::hypot(wxMapCycs[k].x - mx, (wxMapCycs[k].y - my) * 0.5f);
                    if (d < best) { best = d; wxMapHover = (int)k; }
                }
                if (inp.lmbPressed && wxMapHover >= 0)
                    wxSelCyclone = wxMapCycs[(size_t)wxMapHover].id;
            }
        }
        CLAY(CLAY_ID("WxCycMapBack"), {.layout = {.sizing = {CLAY_SIZING_FIXED(W), CLAY_SIZING_FIXED(H)}},
                                       .backgroundColor = Pal::insetBg,
                                       .cornerRadius = CLAY_CORNER_RADIUS(4)})
        {
            if (haveImg)
            {
                CLAY(mid, {.layout = {.sizing = {CLAY_SIZING_FIXED(W), CLAY_SIZING_FIXED(H)}}, .custom = {.customData = &wxMapEl}})
                {
                    CLAY(CLAY_ID("WxCycMapMarks"), {.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0)}},
                                                    .custom = {.customData = &wxMapMarks}}) {}
                }
            }
        }
        CLAY(CLAY_ID("WxCycKey"), {.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0)},
                                              .childGap = 3,
                                              .childAlignment = {.y = CLAY_ALIGN_Y_CENTER},
                                              .layoutDirection = CLAY_LEFT_TO_RIGHT}})
        {
            for (int c = -1; c <= 5; ++c)
                tag(200u + (uint32_t)(c + 1), catShort(c), ssClay(c), Pal::panelSolid);
            text(wxStr(" track: bright = so far, faint = to come"), Pal::textDim, 10);
            CLAY(CLAY_ID("WxCycKeyGap"), {.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0)}}}) {}
            if (uiButton(inp, ui, "WxCycZoom", 0, wxCycZoom ? "World" : "Zoom", wxCycZoom ? "The whole world" : "The selected storm's track"))
                wxCycZoom = !wxCycZoom;
        }
    }

    // ── Active ──
    uiSection("WxCSec", 0, wxStr("ACTIVE NOW (%zu)", wxCycActive.size()));
    if (wxCycActive.empty())
        text("No tropical cyclone anywhere now.", Pal::textDim, 11);
    for (int i = 0; i < (int)wxCycActive.size(); ++i)
    {
        const wx::Cyclone &c = wxCycActive[(size_t)i];
        const float v = c.vmaxAt(now);
        const int cat = catOf(v);
        const glm::dvec3 e = c.earthAt(now);
        const glm::dvec3 e2 = c.earthAt(now + 1800.0);
        const double sp = glm::length(e2 - e) * kRe / 1800.0;
        const size_t hi = std::min((size_t)std::max(0.0, (now - c.t0) / wx::Cyclone::kStepS), c.overLand.size() - 1);
        const bool land = !c.overLand.empty() && c.overLand[hi] != 0;
        bool anyLandfall = false;
        for (size_t k = 1; k < c.overLand.size(); ++k)
            anyLandfall |= c.overLand[k] && !c.overLand[k - 1];
        const double dist = std::acos(std::clamp(glm::dot(e, glm::normalize(glm::dvec3(obsDir))), -1.0, 1.0)) * kRe / 1000.0;
        char age[48];
        fmtRel(c.t0 - now, age, sizeof(age));
        CLAY(CLAY_IDI("WxCRow", (uint32_t)i), {.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0)},
                                                          .padding = {8, 8, 6, 6},
                                                          .childGap = 3,
                                                          .layoutDirection = CLAY_TOP_TO_BOTTOM},
                                               .backgroundColor = c.id == wxSelCyclone ? Pal::rowHighlight : Pal::cardBg,
                                               .cornerRadius = CLAY_CORNER_RADIUS(4),
                                               .border = {.color = Pal::cardBorder, .width = CLAY_BORDER_ALL(1)}})
        {
            if (Clay_Hovered() && inp.lmbPressed)
                wxSelCyclone = c.id;
            CLAY(CLAY_IDI("WxCRowT", (uint32_t)i), {.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0)},
                                                               .childGap = 6,
                                                               .childAlignment = {.y = CLAY_ALIGN_Y_CENTER},
                                                               .layoutDirection = CLAY_LEFT_TO_RIGHT}})
            {
                tag(10u + (uint32_t)i, catShort(cat), ssClay(cat), Pal::panelSolid);
                text(wxStr("%s", c.name.empty() ? "Tropical depression" : c.name.c_str()), Pal::textPrimary, 13);
                text(wxStr("%s, %s", wx::cycloneCategoryName(cat, c.basin), basinShort(c.basin)), Pal::textDim, 11, true);
            }
            text(wxStr("%.0f m/s (%.0f kt), %.0f hPa; moving %s at %.0f m/s; formed %s%s; %.0f km from you", v, v * 1.944f,
                       pressureOf(v), compass(bearingTo(e, e2)), sp, age, land ? "; OVER LAND" : "", dist),
                 Pal::textDim, 11, true);
            CLAY(CLAY_IDI("WxCRowB", (uint32_t)i), {.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0)},
                                                               .childGap = 4,
                                                               .layoutDirection = CLAY_LEFT_TO_RIGHT}})
            {
                static const char *kModes[4] = {"Go to", "Orbit", "Eye", "Landfall"};
                static const char *kTips[4] = {"60 km behind its motion, 1.5 km up, looking at the centre",
                                               "Over the eye from orbit, looking down", "In the eye, 2.5 km up",
                                               "At its next (else first) landfall point on the coast, looking at it"};
                for (int m = 0; m < 4; ++m)
                    if (uiButton(inp, ui, "WxCGo", i * 8 + m, kModes[m], kTips[m], false, m != 3 || anyLandfall))
                    {
                        wxCycPendingAct = 1;
                        wxCycPendingArg = m;
                        wxCycPendingId = c.id;
                    }
                if (uiButton(inp, ui, "WxCWatch", i, "Watch", "Its peak at 1x, from orbit"))
                {
                    wxCycPendingAct = 2;
                    wxCycPendingId = c.id;
                }
            }
        }
    }

    // ── The selected cyclone: its milestones ──
    if (sel)
    {
        const wx::Cyclone &c = *sel;
        const bool alive = now >= c.t0 && now <= c.t0 + c.life;
        const float v = alive ? c.vmaxAt(now) : c.vPeak;
        uiSection("WxCSec", 10, wxStr("%s%s", c.name.empty() ? "TROPICAL DEPRESSION" : c.name.c_str(),
                                       alive ? "" : now < c.t0 ? " (UPCOMING)" : " (PAST)"));
        text(wxStr("%s, season %d; %s %.0f m/s (%.0f kt), %.0f hPa", wx::cycloneBasinName(c.basin), c.year, alive ? "now" : "peak", v, v * 1.944f, pressureOf(v)),
             Pal::textDim, 11, true);
        static std::vector<WxMilestone> ms;
        static uint64_t msFor = 0;
        static double msWall = 0.0;
        if (msFor != c.id || msWall != wxCycWall)
        {
            weatherCycloneMilestones(c, ms);
            msFor = c.id;
            msWall = wxCycWall;
        }
        int next = -1;
        for (int i = 0; i < (int)ms.size(); ++i)
            if (ms[(size_t)i].t > now) { next = i; break; }
        for (int i = 0; i < (int)ms.size(); ++i)
        {
            const WxMilestone &m = ms[(size_t)i];
            char d[48], rel[48];
            fmtDate(m.t, d, sizeof(d), true);
            fmtRel(m.t - now, rel, sizeof(rel));
            CLAY(CLAY_IDI("WxMs", (uint32_t)i), {.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0)},
                                                            .padding = {6, 6, 3, 3},
                                                            .childGap = 6,
                                                            .childAlignment = {.y = CLAY_ALIGN_Y_CENTER},
                                                            .layoutDirection = CLAY_LEFT_TO_RIGHT},
                                                 .backgroundColor = i == next ? Pal::rowHighlight : Pal::cardBg,
                                                 .cornerRadius = CLAY_CORNER_RADIUS(3)})
            {
                CLAY(CLAY_IDI("WxMsT", (uint32_t)i), {.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0)},
                                                                 .childGap = 1,
                                                                 .layoutDirection = CLAY_TOP_TO_BOTTOM}})
                {
                    text(wxStr("%s", m.label.c_str()), m.t <= now ? Pal::textDim : Pal::textPrimary, 11, true);
                    text(wxStr("%s (%s)", d, rel), Pal::textHint, 10);
                }
                if (uiButton(inp, ui, "WxMsSkip", i, m.t <= now ? "Back" : "Skip",
                             m.kind == 6 ? "The clock 2 h before it at 1x, on the coast where it comes in"
                                         : "The clock there at 1x, over the storm from orbit"))
                {
                    wxCycPendingAct = 3;
                    wxCycPendingArg = i;
                    wxCycPendingId = c.id;
                }
            }
        }
    }

    // ── A basin's season ──
    uiSection("WxCSec", 1, "SEASON");
    {
        static const char *kB[6] = {"N Atl", "E Pac", "W Pac", "N Ind", "SW Ind", "Aus/SP"};
        const int b = wxCycBasin >= 0 ? wxCycBasin : (!wxCycActive.empty() ? wxCycActive.front().basin : 0);
        const int p = uiChoiceRow(inp, ui, "WxCBasin", 0, "Basin", kB, 6, b, "The basin whose season is listed");
        if (p >= 0)
        {
            wxCycBasin = p;
            wxCycWall = -1e300;
        }
    }
    if (wxCycSeason.empty())
        text("No systems in this basin within ~200 days.", Pal::textDim, 11);
    for (int i = 0; i < (int)wxCycSeason.size() && i < 40; ++i)
    {
        const wx::Cyclone &c = wxCycSeason[(size_t)i];
        const int pc = catOf(c.vPeak);
        const bool alive = now >= c.t0 && now <= c.t0 + c.life;
        char d0[24], d1[24], rel[48];
        fmtDate(c.t0, d0, sizeof(d0), false);
        fmtDate(c.t0 + c.life, d1, sizeof(d1), false);
        fmtRel(c.t0 - now, rel, sizeof(rel));
        CLAY(CLAY_IDI("WxSeason", (uint32_t)i), {.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0)},
                                                            .padding = {6, 6, 3, 3},
                                                            .childGap = 6,
                                                            .childAlignment = {.y = CLAY_ALIGN_Y_CENTER},
                                                            .layoutDirection = CLAY_LEFT_TO_RIGHT},
                                                 .backgroundColor = c.id == wxSelCyclone ? Pal::rowHighlight : Pal::cardBg,
                                                 .cornerRadius = CLAY_CORNER_RADIUS(3)})
        {
            tag(100u + (uint32_t)i, catShort(pc), ssClay(pc), Pal::panelSolid);
            CLAY(CLAY_IDI("WxSeasonT", (uint32_t)i), {.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0)},
                                                                 .childGap = 1,
                                                                 .layoutDirection = CLAY_TOP_TO_BOTTOM}})
            {
                text(wxStr("%s%s", c.name.empty() ? "(depression)" : c.name.c_str(), alive ? "  ACTIVE" : ""),
                     alive ? Pal::textPrimary : Pal::volValue, 12);
                text(wxStr("%s - %s %d, peak %.0f m/s; %s %s", d0, d1, c.year, c.vPeak, now < c.t0 ? "forms" : "formed", rel),
                     Pal::textHint, 10, true);
            }
            if (uiButton(inp, ui, "WxSeasonShow", i, "Track", "Show its track and milestones", c.id == wxSelCyclone))
                wxSelCyclone = c.id;
            if (uiButton(inp, ui, "WxSeasonWatch", i, "Watch", now < c.t0 ? "Its genesis at 1x, from orbit" : "Its peak at 1x, from orbit"))
            {
                wxCycPendingAct = 2;
                wxCycPendingId = c.id;
            }
        }
    }
    // ── Next genesis per basin ──
    uiSection("WxCSec", 2, "NEXT TO FORM");
    for (int i = 0; i < (int)wxCycNext.size(); ++i)
    {
        const wx::Cyclone &c = wxCycNext[(size_t)i];
        char rel[48];
        fmtRel(c.t0 - now, rel, sizeof(rel));
        uiKV("WxNext", i, basinShort(c.basin),
             wxStr("%s, forms %s (peak %s)", c.name.empty() ? "a depression" : c.name.c_str(), rel,
                   wx::cycloneCategoryName(catOf(c.vPeak), c.basin)));
    }
    text("Every cyclone's whole track is fixed when it forms (a pure function of sim time): the forecast IS its future.",
         Pal::textHint, 10, true);
}

nlohmann::json SatelliteSim::weatherCyclonesJson()
{
    weatherRefreshCyclones(true);
    const double now = (double)simDayJ2000 * kDayS + simSecInDay;
    auto row = [&](const wx::Cyclone &c) {
        const bool alive = now >= c.t0 && now <= c.t0 + c.life;
        const float v = alive ? c.vmaxAt(now) : 0.0f;
        const glm::dvec3 e = c.earthAt(now);
        int landfalls = 0;
        for (size_t k = 1; k < c.overLand.size(); ++k)
            landfalls += c.overLand[k] && !c.overLand[k - 1];
        return nlohmann::json{{"id", std::to_string(c.id)}, {"name", c.name}, {"basin", wx::cycloneBasinName(c.basin)},
                              {"basin_id", c.basin}, {"year", c.year}, {"alive", alive},
                              {"category", alive ? catOf(v) : -2}, {"category_name", alive ? wx::cycloneCategoryName(catOf(v), c.basin) : ""},
                              {"vmax_mps", v}, {"pressure_hpa", alive ? pressureOf(v) : 0.0f},
                              {"lat_deg", latOf(e)}, {"lon_deg", lonOf(e)}, {"genesis_in_h", (c.t0 - now) / 3600.0},
                              {"life_h", c.life / 3600.0}, {"peak_mps", c.vPeak}, {"peak_in_h", (c.tPeak - now) / 3600.0},
                              {"landfalls", landfalls}, {"track_hours", c.track.size()}};
    };
    nlohmann::json j, a = nlohmann::json::array(), s = nlohmann::json::array(), n = nlohmann::json::array();
    for (const wx::Cyclone &c : wxCycActive) a.push_back(row(c));
    for (const wx::Cyclone &c : wxCycSeason) s.push_back(row(c));
    for (const wx::Cyclone &c : wxCycNext) n.push_back(row(c));
    j["active"] = a;
    j["season"] = s;
    j["season_basin"] = wxCycBasin;
    j["next"] = n;
    j["selected"] = std::to_string(wxSelCyclone);
    if (const wx::Cyclone *c = weatherCycloneById(wxSelCyclone))
    {
        std::vector<WxMilestone> ms;
        weatherCycloneMilestones(*c, ms);
        nlohmann::json m = nlohmann::json::array();
        for (const WxMilestone &x : ms)
            m.push_back({{"label", x.label}, {"kind", x.kind}, {"in_h", (x.t - now) / 3600.0}});
        j["milestones"] = m;
    }
    j["map"] = {{"box", {wxMapBox[0], wxMapBox[1], wxMapBox[2], wxMapBox[3]}}, {"ui_image_id", wxMapUiId},
                {"raster_ms", wxMapCpuMs}, {"quads", wxMapQuads.size()}, {"markers", wxMapCycs.size()}};
    return j;
}
