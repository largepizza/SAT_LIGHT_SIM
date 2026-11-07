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
    static const char *k[7] = {tx(Txt::OptTd), tx(Txt::OptTs), "C1", "C2", "C3", "C4", "C5"};
    return k[std::clamp(cat + 1, 0, 6)];
}
const char *basinShort(int b)
{
    static const char *k[6] = {tx(Txt::OptNAtl), tx(Txt::OptEPac), tx(Txt::OptWPac), tx(Txt::OptNInd), tx(Txt::OptSwInd), tx(Txt::OptAusSPac)};
    return (b >= 0 && b < 6) ? k[b] : "?";
}
const char *strengthTerm(int basin)
{
    return basin <= 1 ? tx(Txt::ValHurricaneStrength) : basin == 2 ? tx(Txt::ValTyphoonStrength) : basin == 3 ? tx(Txt::ValVerySevereCyclonicStorm)
                                                                                           : tx(Txt::ValSevereTropicalCyclone);
}
const char *compass(double bearingDeg)
{
    static const char *k[16] = {"N", tx(Txt::OptNne), tx(Txt::OptNe), tx(Txt::OptEne), "E", tx(Txt::OptEse), tx(Txt::OptSe), tx(Txt::OptSse), "S", tx(Txt::OptSsw), tx(Txt::OptSw), tx(Txt::OptWsw), "W", tx(Txt::OptWnw), tx(Txt::OptNw), tx(Txt::OptNnw)};
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
        snprintf(core, sizeof(core), tx(Txt::FmtMin), m);
    if (dt >= 0.0)
        snprintf(b, n, tx(Txt::FmtIn), core);
    else
        snprintf(b, n, tx(Txt::FmtAgo), core);
}
void fmtDate(double tJ2000, char *b, size_t n, bool withTime)
{
    const time_t u = (time_t)std::floor(tJ2000) + 946728000;
    const struct tm *g = gmtime(&u);
    static const char *mon[12] = {tx(Txt::OptJan), tx(Txt::OptFeb), tx(Txt::OptMar), tx(Txt::OptApr), tx(Txt::OptMay), tx(Txt::OptJun), tx(Txt::OptJul), tx(Txt::OptAug), tx(Txt::OptSep), tx(Txt::OptOct), tx(Txt::OptNov), tx(Txt::OptDec)};
    if (!g)
        snprintf(b, n, "--");
    else if (withTime)
        snprintf(b, n, tx(Txt::FmtUtc5), mon[g->tm_mon], g->tm_mday, g->tm_hour, g->tm_min);
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
    for (const auto *lst : {&wxCycActive, &wxCycSeason, &wxCycNext, &wxCycFeatured})
        for (const wx::Cyclone &c : *lst)
            if (c.id == id)
                return &c;
    return nullptr;
}

void SatelliteSim::weatherCycloneMilestones(const wx::Cyclone &c, std::vector<WxMilestone> &out) const
{
    out.clear();
    const size_t n = c.vmax.size();
    out.push_back({c.t0, tx(Txt::TxtGenesisTropicalDepression), 0});
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
        snprintf(b, sizeof(b), "%s %s", c.name.empty() ? tx(Txt::TxtTropicalStorm) : tx(Txt::TxtNamed), c.name.c_str());
        out.push_back({c.t0 + named * wx::Cyclone::kStepS, b, 1});
    }
    if (hur >= 0)
        out.push_back({c.t0 + hur * wx::Cyclone::kStepS, strengthTerm(c.basin), 2});
    if (ri >= 0)
        out.push_back({c.t0 + ri * wx::Cyclone::kStepS, tx(Txt::TxtRapidIntensificationBegins15MS), 3});
    if (major >= 0)
        out.push_back({c.t0 + major * wx::Cyclone::kStepS, tx(Txt::TxtCategory3Major), 4});
    snprintf(b, sizeof(b), tx(Txt::FmtPeakMSKtHpa), wx::cycloneCategoryName(catOf(c.vPeak), c.basin), c.vPeak,
             c.vPeak * 1.944f, pressureOf(c.vPeak));
    out.push_back({c.tPeak, b, 5});
    int nLand = 0;
    for (size_t i = 1; i < c.overLand.size(); ++i)
        if (c.overLand[i] && !c.overLand[i - 1])
        {
            const glm::dvec3 p = c.track[std::min(i, c.track.size() - 1)];
            snprintf(b, sizeof(b), tx(Txt::FmtLandfallAt), ++nLand > 1 ? tx(Txt::TxtAgain) : "", std::abs(latOf(p)),
                     latOf(p) >= 0 ? "N" : "S", std::abs(lonOf(p)), lonOf(p) >= 0 ? "E" : "W", catShort(catOf(c.vmax[i])));
            out.push_back({c.t0 + i * wx::Cyclone::kStepS, b, 6});
        }
    out.push_back({c.t0 + c.life, tx(Txt::TxtDissipation), 7});
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
    FPROF_SCOPE("weatherGoToCyclone");
    const wx::Cyclone *cp = weatherCycloneById(id);
    if (!cp)
    {
        msg = tx(Txt::ValThatCycloneIsNotListed);
        return false;
    }
    const wx::Cyclone c = *cp;   // (the lists may be refreshed below)
    const double t = (double)simDayJ2000 * kDayS + simSecInDay;
    if (mode != 3 && (t < c.t0 || t > c.t0 + c.life))
    {
        msg = (c.name.empty() ? std::string(tx(Txt::TxtIt)) : c.name) + (t < c.t0 ? tx(Txt::TxtHasNotFormedYetWatchJumps)
                                                                       : tx(Txt::TxtHasDissipatedWatchReplaysItsPeak));
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
        what = tx(Txt::ValKmBehindIt);
    }
    else if (mode == 1)
    {
        alt = (float)std::clamp(1.6 * outerRadiusM(c, v), 700000.0, 2500000.0);
        down = true;
        fov = 60.0f;
        what = tx(Txt::ValOverTheEyeFromOrbit);
    }
    else if (mode == 2)
    {
        alt = 2500.0f;
        el = 14.0f;
        // Look across the eye toward the eyewall, ahead of the motion.
        lookAt = c.earthAt(t + 3 * 3600.0);
        what = v >= 33.0f ? tx(Txt::ValInTheEye) : tx(Txt::ValAtTheCentreNoEyeYet);
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
            msg = (c.name.empty() ? std::string("It") : c.name) + tx(Txt::ValNeverMakesLandfall);
            return false;
        }
        standAt = c.track[(size_t)iL];
        lookAt = (t >= c.t0 && t <= c.t0 + c.life) ? e : c.track[(size_t)std::max(0, iL - 6)];
        alt = 150.0f;
        el = 8.0f;
        what = tx(Txt::ValAtItsLandfallPoint);
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
    snprintf(b, sizeof(b), "%s (%s): %s", c.name.empty() ? tx(Txt::TxtTropicalDepression2) : c.name.c_str(),
             wx::cycloneCategoryName(catOf(v), c.basin), what);
    msg = b;
    return true;
}

bool SatelliteSim::weatherWatchCyclone(uint64_t id, std::string &msg)
{
    FPROF_SCOPE("weatherWatchCyclone");
    const wx::Cyclone *cp = weatherCycloneById(id);
    if (!cp)
    {
        msg = tx(Txt::ValThatCycloneIsNotListed);
        return false;
    }
    // Watch (2026-10-24, one button): alive now -> now; else (upcoming or gone) its peak, where it is at its strongest.
    const double now = (double)simDayJ2000 * kDayS + simSecInDay;
    const bool alive = now >= cp->t0 + 3600.0 && now <= cp->t0 + cp->life - 3600.0;
    double t = alive ? now : cp->tPeak;
    // From orbit a storm on the night side is invisible (2026-10-24: a row's Watch drew city lights): forward to its next
    // daylight (the Sun 15 deg up over the centre) while it is still alive, up to 18 h.
    bool dawn = false;
    if (weatherSunSin(cp->earthAt(t), t) < 0.17)
        for (double tt = t + 1800.0; tt <= t + 18.0 * 3600.0 && tt < cp->t0 + cp->life - 3600.0; tt += 1800.0)
            if (weatherSunSin(cp->earthAt(tt), tt) > 0.26)
            {
                t = tt;
                dawn = true;
                break;
            }
    setSimTime(*this, simDayJ2000, simSecInDay, t);
    timePaused = false;
    timeScaleIdx = 0;
    timeDir = 1.0f;
    std::string m;
    const bool ok = weatherGoToCyclone(id, 1, m);
    msg = std::string(dawn ? tx(Txt::TxtInDaylight) : alive ? "" : tx(Txt::TxtAtItsPeak2)) + m;
    return ok;
}

bool SatelliteSim::weatherCycloneMilestone(uint64_t id, int index, std::string &msg)
{
    const wx::Cyclone *cp = weatherCycloneById(id);
    if (!cp)
    {
        msg = tx(Txt::ValThatCycloneIsNotListed);
        return false;
    }
    std::vector<WxMilestone> ms;
    weatherCycloneMilestones(*cp, ms);
    if (index < 0 || index >= (int)ms.size())
    {
        msg = tx(Txt::ValNoSuchMilestone);
        return false;
    }
    const WxMilestone m = ms[(size_t)index];
    const bool land = m.kind == 6;
    // A landfall AT it (2026-10-24, one Watch: it was 2 h before, the eyewall still offshore), from just off the coast;
    // dissipation 30 min before (it is still there); the rest at the moment.
    const double t = land ? m.t : m.kind == 7 ? m.t - 1800.0 : std::max(m.t, cp->t0 + 60.0);
    setSimTime(*this, simDayJ2000, simSecInDay, t);
    timePaused = false;
    timeScaleIdx = 0;
    timeDir = 1.0f;
    std::string g;
    bool ok;
    bool landNight = false;
    if (land)
    {
        // Stand just OFF the coast at THIS landfall's point (2026-10-23): from the track's landfall point toward the storm
        // 2 h out, until the open sea (up to 20 km), 20 m up, looking at it across the surf. On the landfall point itself
        // the view was often a hillside in the rain.
        const wx::Cyclone c = *cp;
        const glm::dvec3 at = c.earthAt(m.t), now = c.earthAt(t), from = c.earthAt(m.t - 7200.0);
        glm::dvec3 stand = at;
        {
            glm::dvec3 dir = from - at * glm::dot(from, at);   // (seaward: where it came from)
            if (glm::length(dir) > 1e-12)
            {
                dir = glm::normalize(dir);
                for (double d = 0.0; d <= 20000.0; d += 1000.0)
                {
                    const double a = d / kRe;
                    const glm::dvec3 p = glm::normalize(at * std::cos(a) + dir * std::sin(a));
                    stand = p;
                    if (oceanFraction(glm::radians(latOf(p)), glm::radians(lonOf(p)), 800.0, 1) > 0.9f) break;
                }
            }
        }
        if (followActive) stopFollow();
        stopTrack();
        setObserverLatLon(latOf(stand), lonOf(stand));
        obsHeightOffset = std::max(0.0f, cpuTerrainHeightM((float)latOf(stand), (float)lonOf(stand))) + 20.0f;
        camera.fovYDeg = std::max(camera.fovYDeg, 70.0f);
        aimCameraAzEl((float)bearingTo(stand, now), 6.0f);
        {   // (It is what it is if the landfall comes at night: say so.)
            const glm::dvec3 si = sunDirEciAt(t);
            const double gg = earthRotationAngle(t);
            const glm::dvec3 se(si.x * std::cos(gg) + si.y * std::sin(gg), -si.x * std::sin(gg) + si.y * std::cos(gg), si.z);
            if (glm::dot(glm::normalize(se), stand) < std::sin(glm::radians(-4.0)))
                landNight = true;
        }
        updatePositions(t);
        trailClearPending = true;
        skyTaaHistValid = false;
        cv2HistoryValid = false;
        wxSelCyclone = c.id;
        g = landNight ? tx(Txt::ValComingAshoreFromJustOffThe2) : tx(Txt::ValComingAshoreFromJustOffThe);
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
    // The raster's CPU copy; each upload takes this frame's staging (ctx.stagingAlloc: frames in flight).
    wxMapStageMapped = std::calloc((size_t)bytes, 1);
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

// ── The weather-map overlay (shaders/weather_map_view.comp) ─────────────────────────────────────────────────────
// The weather cube the march reads, one channel, on the map's box: a storage image the compute pass writes (linear RGBA,
// alpha = the overlay) and the UI draws between the land / sea raster and the tracks.
bool SatelliteSim::weatherOverlayEnsureImage(UIRenderer &ui)
{
    if (wxOvImg != VK_NULL_HANDLE)
        return wxOvUiId != 0;
    if (!ctx_ || wxOvImgFailed)
        return false;
    VulkanContext &ctx = *ctx_;
    try
    {
        ctx.createImage(kWxMapW, kWxMapH, VK_FORMAT_R8G8B8A8_UNORM,
                        VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, wxOvImg,
                        wxOvImgMem);
    }
    catch (const std::exception &e)
    {
        Log::line(std::string("weather map overlay: ") + e.what());
        wxOvImgFailed = true;
        return false;
    }
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = wxOvImg;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = VK_FORMAT_R8G8B8A8_UNORM;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCreateImageView(ctx.device, &vi, nullptr, &wxOvView);
    VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    si.magFilter = si.minFilter = VK_FILTER_LINEAR;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    vkCreateSampler(ctx.device, &si, nullptr, &wxOvSampler);
    VkCommandBuffer cmd = ctx.beginOneTimeCommands();
    ctx.imageBarrier(cmd, wxOvImg, 0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                     VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    const VkClearColorValue clear{{0.0f, 0.0f, 0.0f, 0.0f}};
    const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(cmd, wxOvImg, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear, 1, &range);
    ctx.imageBarrier(cmd, wxOvImg, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    ctx.endOneTimeCommands(cmd);
    wxOvUiId = ui.registerImage(ctx.device, wxOvView, wxOvSampler);
    if (wxOvUiId == 0)
    {
        wxOvImgFailed = true;
        Log::line("weather map overlay: no free UI image slot");
    }
    return wxOvUiId != 0;
}

void SatelliteSim::weatherOverlayRecord(VkCommandBuffer cmd)
{
    if (!wxOvWanted || !ctx_ || wxOvImg == VK_NULL_HANDLE || wxOvFailed)
        return;
    wxOvWanted = false;
    FPROF_SCOPE("weatherOverlayRecord");
    VulkanContext &ctx = *ctx_;
    if (!wxOvPipe)
    {
        if (!cv2MarchDescLayout || !cv2MarchDescSet)
        {
            wxOvFailed = true;
            Log::line("weather map overlay: no cloud march descriptor set (clouds v2 not built)");
            return;
        }
        VkDevice dev = ctx.device;
        VkDescriptorSetLayoutBinding b{0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
        VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        li.bindingCount = 1;
        li.pBindings = &b;
        vkCreateDescriptorSetLayout(dev, &li, nullptr, &wxOvSetLayout);
        VkDescriptorSetLayout lays[2] = {cv2MarchDescLayout, wxOvSetLayout};
        VkPushConstantRange pr{VK_SHADER_STAGE_COMPUTE_BIT, 0, 32};
        VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pli.setLayoutCount = 2;
        pli.pSetLayouts = lays;
        pli.pushConstantRangeCount = 1;
        pli.pPushConstantRanges = &pr;
        vkCreatePipelineLayout(dev, &pli, nullptr, &wxOvPipeLayout);
        VkShaderModule mod = VK_NULL_HANDLE;
        try
        {
            mod = ctx.loadShader("shaders/weather_map_view.comp.spv");
        }
        catch (const std::exception &e)
        {
            Log::line(std::string("weather map overlay: ") + e.what());
            wxOvFailed = true;
            return;
        }
        VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        ci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, mod, "main", nullptr};
        ci.layout = wxOvPipeLayout;
        const VkResult res = vkCreateComputePipelines(dev, ctx.pipelineCache, 1, &ci, nullptr, &wxOvPipe);
        vkDestroyShaderModule(dev, mod, nullptr);
        if (res != VK_SUCCESS)
        {
            Log::line("weather map overlay: vkCreateComputePipelines failed");
            wxOvPipe = VK_NULL_HANDLE;
            wxOvFailed = true;
            return;
        }
        VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1};
        VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        pi.maxSets = 1;
        pi.poolSizeCount = 1;
        pi.pPoolSizes = &ps;
        vkCreateDescriptorPool(dev, &pi, nullptr, &wxOvPool);
        VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        ai.descriptorPool = wxOvPool;
        ai.descriptorSetCount = 1;
        ai.pSetLayouts = &wxOvSetLayout;
        vkAllocateDescriptorSets(dev, &ai, &wxOvSet);
        VkDescriptorImageInfo ii{VK_NULL_HANDLE, wxOvView, VK_IMAGE_LAYOUT_GENERAL};
        VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.dstSet = wxOvSet;
        w.dstBinding = 0;
        w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        w.pImageInfo = &ii;
        frameSafeUpdateDescriptorSets(dev, 1, &w, 0, nullptr);
        Log::line("weather map overlay: pipeline ready");
    }
    // The cloud passes' writes (the weather cube's bake) before this read; the UI's last read of the image before this write.
    VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &mb, 0,
                         nullptr, 0, nullptr);
    ctx.imageBarrier(cmd, wxOvImg, VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_SHADER_WRITE_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                     VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    struct
    {
        glm::vec4 box, opt;
    } pc{glm::vec4((float)wxMapBox[0], (float)wxMapBox[1], (float)wxMapBox[2], (float)wxMapBox[3]),
         glm::vec4((float)wxOvChannel, wxOvOpacity, wxOvMapOnly ? 1.0f : 0.0f, wxOvLod)};
    static_assert(sizeof(pc) == 32, "weather_map_view.comp's push constants");
    VkDescriptorSet sets[2] = {cv2MarchDescSet, wxOvSet};
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, wxOvPipe);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, wxOvPipeLayout, 0, 2, sets, 0, nullptr);
    vkCmdPushConstants(cmd, wxOvPipeLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
    vkCmdDispatch(cmd, (kWxMapW + 7) / 8, (kWxMapH + 7) / 8, 1);
    ctx.imageBarrier(cmd, wxOvImg, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_IMAGE_LAYOUT_GENERAL,
                     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    wxOvValid = true;
    wxOvDispWall = wallNow();
    std::memcpy(wxOvDispBox, wxMapBox, sizeof(wxMapBox));
    wxOvDispChannel = wxOvChannel;
    wxOvDispOpacity = wxOvOpacity;
    wxOvDispMapOnly = wxOvMapOnly;
    wxOvDispLod = wxOvLod;
    ++wxOvDispatches;
}

void SatelliteSim::weatherOverlayDestroy(VkDevice device)
{
    if (wxOvPipe) vkDestroyPipeline(device, wxOvPipe, nullptr);
    if (wxOvPipeLayout) vkDestroyPipelineLayout(device, wxOvPipeLayout, nullptr);
    if (wxOvPool) vkDestroyDescriptorPool(device, wxOvPool, nullptr);
    if (wxOvSetLayout) vkDestroyDescriptorSetLayout(device, wxOvSetLayout, nullptr);
    if (wxOvSampler) vkDestroySampler(device, wxOvSampler, nullptr);
    if (wxOvView) vkDestroyImageView(device, wxOvView, nullptr);
    if (wxOvImg) vkDestroyImage(device, wxOvImg, nullptr);
    if (wxOvImgMem) vkFreeMemory(device, wxOvImgMem, nullptr);
    wxOvPipe = VK_NULL_HANDLE;
    wxOvPipeLayout = VK_NULL_HANDLE;
    wxOvPool = VK_NULL_HANDLE;
    wxOvSetLayout = VK_NULL_HANDLE;
    wxOvSet = VK_NULL_HANDLE;
    wxOvSampler = VK_NULL_HANDLE;
    wxOvView = VK_NULL_HANDLE;
    wxOvImg = VK_NULL_HANDLE;
    wxOvImgMem = VK_NULL_HANDLE;
}

namespace
{
// The overlay's ramps for the legend (= weather_map_view.comp's, display sRGB 0..1).
glm::vec3 wxPoly(const float (&c)[7][3], float t)
{
    t = std::clamp(t, 0.0f, 1.0f);
    glm::vec3 r(0.0f);
    for (int k = 6; k >= 0; --k)
        r = r * t + glm::vec3(c[k][0], c[k][1], c[k][2]);
    return glm::clamp(r, glm::vec3(0.0f), glm::vec3(1.0f));
}
constexpr float kViridis[7][3] = {{0.2777273f, 0.0054073f, 0.3340998f}, {0.1050930f, 1.4046135f, 1.3845902f},
                                  {-0.3308618f, 0.2148476f, 0.0950952f}, {-4.6342305f, -5.7991010f, -19.3324410f},
                                  {6.2282699f, 14.1799334f, 56.6905526f}, {4.7763850f, -13.7451454f, -65.3530326f},
                                  {-5.4354559f, 4.6458526f, 26.3124352f}};
constexpr float kInferno[7][3] = {{0.0002189f, 0.0016510f, -0.0194809f}, {0.1065134f, 0.5639564f, 3.9327124f},
                                  {11.6024931f, -3.9728540f, -15.9423941f}, {-41.7039961f, 17.4363989f, 44.3541452f},
                                  {77.1629357f, -33.4023589f, -81.8073093f}, {-71.3194282f, 32.6260643f, 73.2095199f},
                                  {25.1311262f, -12.2426690f, -23.0703250f}};
glm::vec3 wxOvRamp(int ch, float v)   // ch: SatelliteSim::WxOvChannel
{
    switch (ch)
    {
    case 1: return glm::vec3(v);   // cover
    case 2:  // type
    {
        static const glm::vec3 k[5] = {{0.62f, 0.66f, 0.74f}, {0.30f, 0.72f, 0.88f}, {0.38f, 0.82f, 0.36f},
                                       {0.96f, 0.82f, 0.26f}, {0.92f, 0.26f, 0.24f}};
        const float x = std::clamp(v, 0.0f, 1.0f) * 4.0f;
        const int i = std::min((int)x, 3);
        return glm::mix(k[i], k[i + 1], x - (float)i);
    }
    case 3: return wxPoly(kViridis, 0.15f + 0.85f * v);   // rain
    case 4:  // ground
        return glm::mix(glm::vec3(0.20f, 0.42f, 0.18f), glm::vec3(0.96f, 0.92f, 0.84f), std::sqrt(std::clamp(v, 0.0f, 1.0f)));
    default: return wxPoly(kInferno, 0.25f + 0.75f * v);
    }
}
} // namespace

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
        text(tx(Txt::NoteTropicalCyclonesAreOffSettingsWeather), Pal::textDim, 11, true);
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
        // The weather-map overlay: asked for at most 4 times a second while a box / channel / look changed, else twice a
        // second (the cube re-bakes as sim time moves); recordCompute dispatches it after the clouds' pass.
        const bool ovOn = haveImg && wxOvChannel != WxOvOff && weatherOverlayEnsureImage(ui);
        if (ovOn)
        {
            const bool changed = !wxOvValid || std::memcmp(wxOvDispBox, wxMapBox, sizeof(wxMapBox)) != 0 ||
                                 wxOvDispChannel != wxOvChannel || wxOvDispOpacity != wxOvOpacity ||
                                 wxOvDispMapOnly != wxOvMapOnly || wxOvDispLod != wxOvLod;
            if (wallNow() - wxOvDispWall >= (changed ? 0.25 : 0.5))
                wxOvWanted = true;
        }
        wxOvEl.imageId = wxOvUiId;
        auto marks = [&]() {
            CLAY(CLAY_ID("WxCycMapMarks"), {.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0)}},
                                            .custom = {.customData = &wxMapMarks}}) {}
        };
        CLAY(CLAY_ID("WxCycMapBack"), {.layout = {.sizing = {CLAY_SIZING_FIXED(W), CLAY_SIZING_FIXED(H)}},
                                       .backgroundColor = Pal::insetBg,
                                       .cornerRadius = CLAY_CORNER_RADIUS(4)})
        {
            if (haveImg)
            {
                CLAY(mid, {.layout = {.sizing = {CLAY_SIZING_FIXED(W), CLAY_SIZING_FIXED(H)}}, .custom = {.customData = &wxMapEl}})
                {
                    if (ovOn && wxOvValid)   // between the land / sea raster and the tracks
                    {
                        CLAY(CLAY_ID("WxCycMapOv"), {.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0)}},
                                                     .custom = {.customData = &wxOvEl}})
                        {
                            marks();
                        }
                    }
                    else
                        marks();
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
            text(wxStr(tx(Txt::FmtTrackBrightSoFarFaintTo)), Pal::textDim, 10);
            CLAY(CLAY_ID("WxCycKeyGap"), {.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0)}}}) {}
            if (uiButton(inp, ui, "WxCycZoom", 0, wxCycZoom ? tx(Txt::BtnWorld) : tx(Txt::LblZoom), wxCycZoom ? tx(Txt::TipTheWholeWorld) : tx(Txt::TipTheSelectedStormsTrack)))
                wxCycZoom = !wxCycZoom;
        }
        // The overlay's controls: the map's channels, the convection cube's, the look, the mip; then the channel's key.
        {
            const char *mapOpts[5] = {tx(Txt::LblOff), tx(Txt::OptCover), tx(Txt::OptType), tx(Txt::SldRain), tx(Txt::OptGround)};
            const char *cvOpts[4] = {tx(Txt::OptCells), tx(Txt::OptTops), tx(Txt::OptAnvil), tx(Txt::OptOutflow)};
            const int pm = uiChoiceRow(inp, ui, "WxOvMap", 0, tx(Txt::LblWeatherMap), mapOpts, 5,
                                       wxOvChannel <= WxOvGround ? wxOvChannel : -1, tx(Txt::TipWeatherMapOverlay));
            if (pm >= 0) wxOvChannel = pm;
            const int pc = uiChoiceRow(inp, ui, "WxOvMap", 1, tx(Txt::LblConvection), cvOpts, 4,
                                       wxOvChannel >= WxOvConv ? wxOvChannel - WxOvConv : -1, tx(Txt::TipWeatherMapConvection));
            if (pc >= 0) wxOvChannel = WxOvConv + pc;
            if (wxOvChannel != WxOvOff)
            {
                const char *opOpts[5] = {tx(Txt::Opt25Pct), tx(Txt::Opt50Pct), tx(Txt::Opt75Pct), tx(Txt::Opt100Pct), tx(Txt::OptMapOnly)};
                const int opCur = wxOvMapOnly ? 4 : std::clamp((int)std::lround(wxOvOpacity * 4.0f) - 1, 0, 3);
                const int po = uiChoiceRow(inp, ui, "WxOvOpac", 0, tx(Txt::LblOpacity), opOpts, 5, opCur);
                if (po >= 0) { wxOvMapOnly = po == 4; if (po < 4) wxOvOpacity = 0.25f * (float)(po + 1); }
                const char *lodOpts[6] = {tx(Txt::OptAuto), "0", "1", "2", "3", "4"};
                const int lodCur = wxOvLod < 0.0f ? 0 : std::clamp((int)wxOvLod + 1, 1, 5);
                const int pl = uiChoiceRow(inp, ui, "WxOvMip", 0, tx(Txt::LblMip), lodOpts, 6, lodCur, tx(Txt::TipWeatherMapMip));
                if (pl >= 0) wxOvLod = pl == 0 ? -1.0f : (float)(pl - 1);
                static const Txt kNotes[WxOvCount] = {Txt::LblOff, Txt::NoteWxOvCover, Txt::NoteWxOvType, Txt::NoteWxOvRain,
                                                      Txt::NoteWxOvGround, Txt::NoteWxOvCells, Txt::NoteWxOvTops,
                                                      Txt::NoteWxOvAnvil, Txt::NoteWxOvOutflow};
                CLAY(CLAY_ID("WxOvKey"), {.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0)},
                                                     .childGap = 6,
                                                     .childAlignment = {.y = CLAY_ALIGN_Y_CENTER},
                                                     .layoutDirection = CLAY_LEFT_TO_RIGHT}})
                {
                    CLAY(CLAY_ID("WxOvRamp"), {.layout = {.sizing = {CLAY_SIZING_FIT(0), CLAY_SIZING_FIT(0)},
                                                          .layoutDirection = CLAY_LEFT_TO_RIGHT}})
                    {
                        for (int k = 0; k < 12; ++k)
                        {
                            const glm::vec3 c = wxOvRamp(wxOvChannel, (k + 0.5f) / 12.0f) * 255.0f;
                            CLAY(CLAY_IDI("WxOvSw", (uint32_t)k), {.layout = {.sizing = {CLAY_SIZING_FIXED(8), CLAY_SIZING_FIXED(10)}},
                                                                   .backgroundColor = {c.r, c.g, c.b, 255.0f}}) {}
                        }
                    }
                    text(tx(kNotes[std::clamp(wxOvChannel, 0, (int)WxOvCount - 1)]), Pal::textDim, 10, true);
                }
            }
        }
    }

    // ── Active ──
    uiSection("WxCSec", 0, wxStr(tx(Txt::FmtActiveNow), wxCycActive.size()));
    if (wxCycActive.empty())
        text(tx(Txt::NoteNoTropicalCycloneAnywhereNow), Pal::textDim, 11);
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
                text(wxStr("%s", c.name.empty() ? tx(Txt::TxtTropicalDepression2) : c.name.c_str()), Pal::textPrimary, 13);
                text(wxStr("%s, %s", wx::cycloneCategoryName(cat, c.basin), basinShort(c.basin)), Pal::textDim, 11, true);
            }
            text(wxStr(tx(Txt::FmtMSKtHpaMovingAt), v, v * 1.944f,
                       pressureOf(v), compass(bearingTo(e, e2)), sp, age, land ? tx(Txt::TxtOverLand) : "", dist),
                 Pal::textDim, 11, true);
            CLAY(CLAY_IDI("WxCRowB", (uint32_t)i), {.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0)},
                                                               .childGap = 4,
                                                               .layoutDirection = CLAY_LEFT_TO_RIGHT}})
            {
                (void)anyLandfall;
                if (uiButton(inp, ui, "WxCWatch", i, tx(Txt::BtnWatch), tx(Txt::TipTheWholeStormNowFromOrbit)))
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
        uiSection("WxCSec", 10, wxStr("%s%s", c.name.empty() ? tx(Txt::TxtTropicalDepression) : c.name.c_str(),
                                       alive ? "" : now < c.t0 ? tx(Txt::TxtUpcoming) : tx(Txt::TxtPast)));
        text(wxStr(tx(Txt::FmtSeasonMSKtHpa), wx::cycloneBasinName(c.basin), c.year, alive ? "now" : "peak", v, v * 1.944f, pressureOf(v)),
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
                if (uiButton(inp, ui, "WxMsSkip", i, tx(Txt::BtnWatch),
                             m.kind == 6 ? tx(Txt::TipItComingAshoreFromJustOff) : tx(Txt::TipThatMomentTheStormFromOrbit)))
                {
                    wxCycPendingAct = 3;
                    wxCycPendingArg = i;
                    wxCycPendingId = c.id;
                }
            }
        }
    }

    // ── A basin's season ──
    uiSection("WxCSec", 1, tx(Txt::SecSeason));
    {
        static const char *kB[6] = {tx(Txt::OptNAtl), tx(Txt::OptEPac), tx(Txt::OptWPac), tx(Txt::OptNInd), tx(Txt::OptSwInd), tx(Txt::OptAusSp)};
        const int b = wxCycBasin >= 0 ? wxCycBasin : (!wxCycActive.empty() ? wxCycActive.front().basin : 0);
        const int p = uiChoiceRow(inp, ui, "WxCBasin", 0, tx(Txt::LblBasin), kB, 6, b, tx(Txt::TipTheBasinWhoseSeasonIsListed));
        if (p >= 0)
        {
            wxCycBasin = p;
            wxCycWall = -1e300;
        }
    }
    if (wxCycSeason.empty())
        text(tx(Txt::NoteNoSystemsInThisBasinWithin), Pal::textDim, 11);
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
                text(wxStr("%s%s", c.name.empty() ? tx(Txt::TxtDepression) : c.name.c_str(), alive ? tx(Txt::TxtActive) : ""),
                     alive ? Pal::textPrimary : Pal::volValue, 12);
                text(wxStr(tx(Txt::FmtPeakMS), d0, d1, c.year, c.vPeak, now < c.t0 ? "forms" : "formed", rel),
                     Pal::textHint, 10, true);
            }
            if (Clay_Hovered() && inp.lmbPressed)
                wxSelCyclone = c.id;   // (a click on the row shows its track and milestones)
            if (uiButton(inp, ui, "WxSeasonWatch", i, tx(Txt::BtnWatch), alive ? tx(Txt::TipTheWholeStormNowFromOrbit) : tx(Txt::TipTheStormAtItsPeakFrom)))
            {
                wxCycPendingAct = 2;
                wxCycPendingId = c.id;
            }
        }
    }
    // ── Next genesis per basin ──
    uiSection("WxCSec", 2, tx(Txt::SecNextToForm));
    for (int i = 0; i < (int)wxCycNext.size(); ++i)
    {
        const wx::Cyclone &c = wxCycNext[(size_t)i];
        char rel[48];
        fmtRel(c.t0 - now, rel, sizeof(rel));
        uiKV("WxNext", i, basinShort(c.basin),
             wxStr(tx(Txt::FmtFormsPeak), c.name.empty() ? tx(Txt::TxtADepression) : c.name.c_str(), rel,
                   wx::cycloneCategoryName(catOf(c.vPeak), c.basin)));
    }
    text(tx(Txt::NoteEveryCyclonesWholeTrackIsFixed),
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
                {"raster_ms", wxMapCpuMs}, {"quads", wxMapQuads.size()}, {"markers", wxMapCycs.size()},
                {"overlay", {{"channel", wxOvChannel}, {"opacity", wxOvOpacity}, {"map_only", wxOvMapOnly}, {"lod", wxOvLod},
                             {"ui_image_id", wxOvUiId}, {"valid", wxOvValid}, {"failed", wxOvFailed || wxOvImgFailed},
                             {"dispatches", wxOvDispatches}}}};
    return j;
}
