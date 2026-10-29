// SatelliteSimWeatherWindow.cpp — the Weather window (2026-10-23, .plans/WEATHER_EVENTS_PLAN.md "Addendum: weather
// like missions").
//
// The storm objects (Weather.h: every storm a pure function of sim time) seen like the Missions window. Opened from the
// weather-cloud button on the right HUD panel (or N, KB_WEATHER; harness `ui open weather`, `weather ...`). Tabs:
//   Radar    — a local precipitation radar centred on the observer, north up, 50 / 150 / 400 km: NWS-style
//              reflectivity colours (rain green .. yellow .. red .. magenta; snow blue; mixed pink), range rings, you,
//              the storms' markers (click one for its row), an optional loop of the frames taken while it was open.
//              THE RAIN IS THE RENDERER'S: shaders/weather_radar.comp asks cv2Field (clouds_v2.glsl), the field the
//              cloud march draws, for the rain a few hundred metres over the ground in every cell — the weather cube's
//              precipitation through the same flowed, drifted lookup (cv2FieldLow's rain branch) and the storms' rain
//              cores under their dominant towers (gCv2RainCore from cv2ColumnSigma) — like the lightning pass's rain
//              map at the eye. 256 x 256 cells, dispatched at most 4 times a second while the radar is shown (and only
//              when the time, the place or the range changed), read back the next frame and coloured here.
//   Storms   — the storms alive near you and the strongest worldwide: kind, stage (forming / growing / mature /
//              decaying, from the strongest tower's lifecycle), age and time left, distance and bearing, an estimate of
//              the lightning (the lightning pass's rate x its dominant towers' activity). Go to: on the ground ~50 km
//              upwind of its strongest tower, looking at it (the harness's `storms go`). Watch: its mature stage at 1x,
//              then Go to.
//   Controls — the storm-object settings (also in Settings > Weather).
// Nothing here runs while the window is closed; the rows are rebuilt at most twice a second.
#include "../FrameProfile.h"
#include "SatelliteSim.h"
#include "SatPhotometry.h"
#include "../UIRenderer.h"
#include "../VulkanContext.h"
#include "../Log.h"
#include "clay.h"
#include "UIPalette.h"
#include "../UiTextClay.h"

#include <nlohmann/json.hpp>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <ctime>

namespace
{
constexpr double kRe = 6371000.0;
constexpr double kWxNearKm = 1200.0;   // the Storms tab's "near you"
constexpr int kWxNearMax = 12, kWxWorldMax = 10;
constexpr int kWxNearSlots = 4;        // the Storms tab's sticky "near you" slots
constexpr int kWxLoopMax = 10;         // the loop's frames
constexpr double kWxLoopStepS = 300.0; // sim seconds between loop frames
constexpr float kWxSampleAglM = 120.0f; // the radar's sample over the (smoothed) ground

double wallNow() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
Clay_String cs(const char *s) { return Clay_String{false, (int32_t)strlen(s), s}; }

glm::vec4 palV(const Clay_Color &c, float a = -1.0f)
{
    return glm::vec4(c.r / 255.0f, c.g / 255.0f, c.b / 255.0f, a >= 0.0f ? a : c.a / 255.0f);
}

void fmtDur(double s, char *b, size_t n)
{
    s = std::max(s, 0.0);
    const long long m = (long long)std::floor(s / 60.0);
    if (m >= 60)
        snprintf(b, n, "%lldh %02lldm", m / 60, m % 60);
    else
        snprintf(b, n, tx(Txt::FmtMin), m);
}

const char *kindName(int k)
{
    switch (k)
    {
    case (int)wx::StormKind::AirMass: return tx(Txt::ValAirMassCell);
    case (int)wx::StormKind::Multicell: return tx(Txt::ValMulticell);
    case (int)wx::StormKind::Supercell: return tx(Txt::ValSupercell);
    case (int)wx::StormKind::SquallLine: return tx(Txt::ValSquallLine);
    case (int)wx::StormKind::Mcs: return tx(Txt::ValMesoscaleConvectiveSystem);
    case (int)wx::StormKind::Tropical: return tx(Txt::ValTropicalCyclone);
    default: return tx(Txt::ValStorm); // a kind this window does not know yet
    }
}
const char *kindShort(int k)
{
    switch (k)
    {
    case (int)wx::StormKind::AirMass: return tx(Txt::ValCell);
    case (int)wx::StormKind::Multicell: return tx(Txt::ValMulti);
    case (int)wx::StormKind::Supercell: return tx(Txt::ValSuper);
    case (int)wx::StormKind::SquallLine: return tx(Txt::ValLine);
    case (int)wx::StormKind::Mcs: return tx(Txt::ValMcs);
    case (int)wx::StormKind::Tropical: return tx(Txt::ValTc);
    default: return tx(Txt::ValStorm);
    }
}
const char *stageName(int s)
{
    static const char *k[4] = {"forming", "growing", "mature", "decaying"};
    return k[std::clamp(s, 0, 3)];
}
double wwLat(const glm::dvec3 &e) { return glm::degrees(std::asin(std::clamp(e.z, -1.0, 1.0))); }
double wwLon(const glm::dvec3 &e) { return glm::degrees(std::atan2(e.y, e.x)); }
const char *compass(double bearingDeg)
{
    static const char *k[16] = {"N", tx(Txt::OptNne), tx(Txt::OptNe), tx(Txt::OptEne), "E", tx(Txt::OptEse), tx(Txt::OptSe), tx(Txt::OptSse), "S", tx(Txt::OptSsw), tx(Txt::OptSw), tx(Txt::OptWsw), "W", tx(Txt::OptWnw), tx(Txt::OptNw), tx(Txt::OptNnw)};
    const int i = (int)std::floor(std::fmod(bearingDeg + 360.0 + 11.25, 360.0) / 22.5) % 16;
    return k[i];
}

// sRGB colour ramps (stops in dBZ): the NWS reflectivity scale for rain, its winter blues for snow, pinks for a mix.
struct Stop { float dbz; uint8_t r, g, b; };
const Stop kRain[] = {{8, 120, 210, 125}, {15, 60, 190, 70}, {20, 20, 160, 30}, {25, 10, 120, 20}, {30, 240, 235, 40},
                      {35, 230, 190, 20}, {40, 250, 140, 10}, {45, 240, 40, 20}, {50, 200, 10, 10}, {55, 150, 0, 0},
                      {60, 250, 40, 250}, {65, 160, 90, 220}, {70, 255, 255, 255}};
const Stop kSnow[] = {{8, 175, 220, 250}, {20, 100, 165, 240}, {30, 45, 95, 225}, {40, 30, 40, 170}, {50, 130, 60, 210}};
const Stop kMix[] = {{8, 250, 195, 225}, {25, 240, 125, 195}, {40, 210, 40, 150}, {55, 150, 0, 110}};
template <size_t N>
glm::vec3 ramp(const Stop (&s)[N], float dbz)
{
    if (dbz <= s[0].dbz)
        return glm::vec3(s[0].r, s[0].g, s[0].b);
    for (size_t i = 1; i < N; ++i)
        if (dbz <= s[i].dbz)
        {
            const float f = (dbz - s[i - 1].dbz) / (s[i].dbz - s[i - 1].dbz);
            return glm::mix(glm::vec3(s[i - 1].r, s[i - 1].g, s[i - 1].b), glm::vec3(s[i].r, s[i].g, s[i].b), f);
        }
    return glm::vec3(s[N - 1].r, s[N - 1].g, s[N - 1].b);
}

// The radar's rain unit (rate x shaft x (1 + core), cv2FieldLow: ~1 = heavy rain) as reflectivity: R = 60 rr^1.5 mm/h
// (the rain particles' Marshall-Palmer rate, CLAUDE.md "Rain / sleet / snow"), Z = 200 R^1.6.
float rainDbz(float rr)
{
    const float R = 60.0f * std::pow(std::max(rr, 1e-5f), 1.5f);
    return 23.0f + 16.0f * std::log10(R);
}
constexpr float kDbzMin = 8.0f; // nothing drawn below (~0.015 of the rain unit)
// rainDbz(rr) >= d  <=>  rr >= rainForDbz(d)
float rainForDbz(float d) { return std::pow(std::pow(10.0f, (d - 23.0f) / 16.0f) / 60.0f, 1.0f / 1.5f); }
} // namespace

const char *SatelliteSim::wxStr(const char *fmt, ...)
{
    char b[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof(b), fmt, ap);
    va_end(ap);
    wxText_.emplace_back(b);
    return wxText_.back().c_str();
}

// Map frame (the drifted sphere the storms live in) -> Earth-fixed, at time t (cv2Drift turns Earth-fixed by +phase).
glm::dvec3 SatelliteSim::weatherMapToEarth(const glm::dvec3 &d, double t) const
{
    const double ph = (double)cloudDriftRate * (t - kCloudDriftEpochS) + cloudDriftPhaseOffset;
    const double c = std::cos(ph), s = std::sin(ph);
    return glm::normalize(glm::dvec3(d.x * c + d.y * s, -d.x * s + d.y * c, d.z));
}

// ── The storms ──────────────────────────────────────────────────────────────────────────────────────────────────
// A storm's state at t: the strongest tower now, its stage, the lightning estimate, the moment worth watching.
void SatelliteSim::weatherStormState(const wx::Storm &st, double t, float lightningRate, WxStormRow &r)
{
    r.id = st.id;
    r.kind = (int)st.kind;
    r.t0 = st.t0;
    r.tEnd = st.t0 + (double)st.life;
    r.towerCount = st.towerCount;
    float best = 0.0f;
    int bestK = -1, domK = -1;
    float domPeak = -1.0f;
    r.towersAlive = 0;
    r.flashesPerMin = 0.0f;
    const double age = t - st.t0;
    float bestAge = 0.0f, bestLife = 0.0f;
    for (int k = 0; k < (int)st.towerCount; ++k)   // (towerCount: up to 16 since the line systems)
    {
        const wx::TowerSpec &ts = st.towers[k];
        float s = 0.0f, cAge = 0.0f, cLife = 0.0f, shift = 0.0f;
        uint32_t sd = 0;
        wx::Catalog::towerCycleAt(ts, age, s, cAge, cLife, sd, shift);   // (a plain tower: its own strength and age)
        if (s > 0.0f)
            ++r.towersAlive;
        if (s > best)
        {
            best = s;
            bestK = k;
            bestAge = cAge;
            bestLife = cLife;
        }
        if (ts.role >= 1.0f)
        {
            // cloud_v2_lightning.comp: a dominant tower flashes at "Lightning rate" x smoothstep(0.35, 0.75, strength).
            const float a = std::clamp((s - 0.35f) / 0.4f, 0.0f, 1.0f);
            r.flashesPerMin += lightningRate * a * a * (3.0f - 2.0f * a);
            if (ts.peak > domPeak)
            {
                domPeak = ts.peak;
                domK = k;
            }
        }
    }
    r.strength = best;
    const bool line = wx::isLineSystem(st.kind);
    if (line && domK >= 0)
    {
        // A line system's stage is its envelope's (the cells regenerate): organising, mature, weakening.
        const wx::TowerSpec &ts = st.towers[domK];
        r.stage = age < ts.sysOn ? 0 : age < ts.sysOn + ts.sysRamp ? 1 : age < ts.sysOff - ts.sysRamp ? 2 : 3;
    }
    else if (bestK < 0)
        r.stage = age < 0.0 || (domK >= 0 && age < st.towers[domK].t0) ? 0 : 3;
    else
    {
        const wx::TowerSpec &ts = st.towers[bestK];
        r.stage = bestAge < ts.growS ? 1 : (bestAge > bestLife - ts.decayS ? 3 : 2);
    }
    // Worth watching: the dominant tower grown and its anvil spread (~45 min after it has grown: SatelliteSimWeather.cpp
    // updateStorms' "mature"), but before it decays.
    if (domK < 0)
        domK = std::max(bestK, 0);
    if (line && st.towerCount > 0)
    {
        const wx::TowerSpec &ts = st.towers[domK];
        const double a = ts.sysOn + ts.sysRamp, b = std::max(a, (double)ts.sysOff - ts.sysRamp);
        r.tMature = st.t0 + 0.5 * (a + b);
    }
    else if (st.towerCount > 0)
    {
        const wx::TowerSpec &ts = st.towers[domK];
        const double grown = ts.t0 + ts.growS;
        const double decay = ts.t0 + std::max(ts.life - ts.decayS, ts.growS);
        r.tMature = st.t0 + std::clamp(grown + 1800.0, grown, std::max(grown, decay - 300.0));
    }
    else
        r.tMature = st.t0;
    r.score = best * (line ? 2.0f : r.kind == (int)wx::StormKind::Supercell ? 1.6f : r.kind == (int)wx::StormKind::AirMass ? 1.0f : 1.3f)
            + 0.05f * (float)r.towersAlive;
}

bool SatelliteSim::weatherStormNow(uint64_t id, double t, WxStormRow &out)
{
    if (!stormCatalog)
        return false;
    std::vector<const wx::Storm *> v;
    stormCatalog->stormsAt(t, v);
    for (const wx::Storm *st : v)
        if (st->id == id)
        {
            weatherStormState(*st, t, cv2LightningRate, out);
            out.e = weatherMapToEarth(st->centreAt(t), t);
            const glm::dvec3 oe = glm::normalize(glm::dvec3(obsDir));
            out.distKm = std::acos(std::clamp(glm::dot(out.e, oe), -1.0, 1.0)) * kRe / 1000.0;
            const glm::dvec3 E = glm::normalize(glm::cross(glm::dvec3(0, 0, 1), oe) + glm::dvec3(1e-12, 0, 0));
            const glm::dvec3 N = glm::cross(oe, E);
            const glm::dvec3 to = out.e - oe * glm::dot(out.e, oe);
            out.bearingDeg = std::fmod(glm::degrees(std::atan2(glm::dot(to, E), glm::dot(to, N))) + 360.0, 360.0);
            return true;
        }
    return false;
}

void SatelliteSim::weatherRefreshRows(bool force)
{
    FPROF_SCOPE("weatherRefreshRows");
    const double now = (double)simDayJ2000 * 86400.0 + simSecInDay;
    const double wall = wallNow();
    if (!force && wall - wxRowsWall < 0.5 && std::abs(now - wxRowsSim) < 1800.0)
        return;
    wxRowsWall = wall;
    wxRowsSim = now;
    wxNear.clear();
    wxWorld.clear();
    wxStormsAlive = 0;
    if (!stormCatalog || !stormObjects)
        return;
    std::vector<const wx::Storm *> v;
    stormCatalog->stormsAt(now, v);
    const glm::dvec3 oe = glm::normalize(glm::dvec3(obsDir));
    const glm::dvec3 E = glm::normalize(glm::cross(glm::dvec3(0, 0, 1), oe) + glm::dvec3(1e-12, 0, 0));
    const glm::dvec3 N = glm::cross(oe, E);
    std::vector<WxStormRow> all;
    all.reserve(v.size());
    for (const wx::Storm *st : v)
    {
        WxStormRow r;
        weatherStormState(*st, now, cv2LightningRate, r);
        if (r.towersAlive == 0 && r.stage != 0)
            continue;
        r.e = weatherMapToEarth(st->centreAt(now), now);
        r.distKm = std::acos(std::clamp(glm::dot(r.e, oe), -1.0, 1.0)) * kRe / 1000.0;
        const glm::dvec3 to = r.e - oe * glm::dot(r.e, oe);
        r.bearingDeg = std::fmod(glm::degrees(std::atan2(glm::dot(to, E), glm::dot(to, N))) + 360.0, 360.0);
        all.push_back(r);
    }
    wxStormsAlive = (int)all.size();
    weatherFeaturedNow(all);
    // The Storms tab's "near you": kWxNearSlots STICKY slots of organised storms (a storm keeps its slot while it is
    // near and alive; a free slot takes the strongest one not shown), so rows do not jump about as storms come and go.
    // Air-mass cells only when nothing organised is near.
    {
        std::vector<const WxStormRow *> cand;
        bool organised = false;
        for (const WxStormRow &r : all)
            if (r.distKm <= kWxNearKm && r.kind != (int)wx::StormKind::AirMass)
                organised = true;
        for (const WxStormRow &r : all)
            if (r.distKm <= kWxNearKm && (!organised || r.kind != (int)wx::StormKind::AirMass))
                cand.push_back(&r);
        std::sort(cand.begin(), cand.end(), [](const WxStormRow *a, const WxStormRow *b) {
            return a->score != b->score ? a->score > b->score : a->id < b->id;
        });
        auto findCand = [&](uint64_t id) -> const WxStormRow * {
            for (const WxStormRow *r : cand)
                if (r->id == id) return r;
            return nullptr;
        };
        std::vector<uint64_t> ids;
        for (uint64_t id : wxNearSlotIds)
            ids.push_back(findCand(id) ? id : 0);   // kept in place, or freed
        ids.resize(kWxNearSlots, 0);
        for (const WxStormRow *r : cand)
        {
            if (std::find(ids.begin(), ids.end(), r->id) != ids.end()) continue;
            auto it = std::find(ids.begin(), ids.end(), (uint64_t)0);
            if (it == ids.end()) break;
            *it = r->id;
        }
        wxNearSlotIds = ids;
        wxNearSlots.assign(kWxNearSlots, WxStormRow{});
        for (int i = 0; i < kWxNearSlots; ++i)
            if (const WxStormRow *r = ids[(size_t)i] ? findCand(ids[(size_t)i]) : nullptr)
                wxNearSlots[(size_t)i] = *r;
    }
    for (const WxStormRow &r : all)
        if (r.distKm <= kWxNearKm)
            wxNear.push_back(r);
    std::sort(wxNear.begin(), wxNear.end(), [](const WxStormRow &a, const WxStormRow &b) { return a.distKm < b.distKm; });
    if ((int)wxNear.size() > kWxNearMax)
        wxNear.resize(kWxNearMax);
    // Worldwide: organised storms by default (any kind but the single air-mass cell: a new kind counts as organised).
    for (const WxStormRow &r : all)
        if (wxWorldAllKinds || r.kind != (int)wx::StormKind::AirMass)
            wxWorld.push_back(r);
    const size_t k = std::min<size_t>(kWxWorldMax, wxWorld.size());
    std::partial_sort(wxWorld.begin(), wxWorld.begin() + (long)k, wxWorld.end(),
                      [](const WxStormRow &a, const WxStormRow &b) { return a.score != b.score ? a.score > b.score : a.id < b.id; });
    wxWorld.resize(k);
    for (WxStormRow &r : wxNear)
        weatherStormMilestones(r);
    for (WxStormRow &r : wxWorld)
        weatherStormMilestones(r);
    for (WxStormRow &r : wxNearSlots)
        if (r.id != 0)
            weatherStormMilestones(r);
}

// ── Storm milestones (2026-10-23): a supercell's tornado, a line's passage over you ──────────────────────────────────
void SatelliteSim::weatherStormMilestones(WxStormRow &r)
{
    r.tTorn0 = r.tTorn1 = r.tPass = 0.0;
    const wx::Storm *st = stormCatalog ? stormCatalog->find(r.id) : nullptr;
    if (!st)
        return;
    const double now = (double)simDayJ2000 * 86400.0 + simSecInDay;
    TornadoNow T;
    if (st->kind == wx::StormKind::Supercell && tornadoOf(*st, now, T) && !T.spout)
    {
        r.tTorn0 = T.t0;
        r.tTorn1 = T.t1;
    }
    if (st->kind == wx::StormKind::SquallLine || st->kind == wx::StormKind::Mcs)
        r.tPass = weatherLinePassage(*st, now);
}

// When the line's convective edge reaches the observer (where they stand now, Earth-fixed): the observer's distance ahead
// of the line — across it along the motion, less the towers' bow at its place along the line (bow x (1 - x^2)) — falls
// through zero while it is within the line's length. 5-min steps over the convective life, then bisected. 0: none.
double SatelliteSim::weatherLinePassage(const wx::Storm &st, double from) const
{
    const glm::dvec3 oe = glm::normalize(glm::dvec3(obsDir));
    const double t1 = st.t0 + (double)st.sysLife;
    const double ramp = (double)st.towers[0].sysRamp;
    double t = std::max(from, st.t0 + 0.5 * ramp);
    if (t >= t1)
        return 0.0;
    auto ahead = [&](double tt, double &alongN) {
        const double ph = (double)cloudDriftRate * (tt - kCloudDriftEpochS) + cloudDriftPhaseOffset;
        const double c = std::cos(ph), s = std::sin(ph);
        const glm::dvec3 m(oe.x * c - oe.y * s, oe.x * s + oe.y * c, oe.z);   // the observer in the map frame
        const glm::dvec3 ctr = st.centreAt(tt);
        glm::dvec3 v = st.centreAt(tt + 60.0) - ctr;
        v -= ctr * glm::dot(v, ctr);
        if (glm::length(v) < 1e-15) { alongN = 9.0; return 1e9; }
        v = glm::normalize(v);
        const glm::dvec3 e = glm::normalize(glm::cross(glm::dvec3(0, 0, 1), ctr) + glm::dvec3(1e-12, 0, 0));
        const glm::dvec3 n = glm::cross(ctr, e);
        const glm::dvec3 L = e * std::cos((double)st.lineAz) + n * std::sin((double)st.lineAz);
        const glm::dvec3 d = (m - ctr) * kRe;
        alongN = glm::dot(d, L) / std::max((double)st.lineHalfM, 1.0);
        const double x = std::clamp(alongN, -1.0, 1.0);
        return glm::dot(d, v) - (double)st.bow * (1.0 - x * x);
    };
    double alongN = 0.0;
    double g0 = ahead(t, alongN);
    if (g0 <= 0.0)
        return 0.0;   // already behind it (or passed)
    for (double tt = t + 300.0; tt <= t1; tt += 300.0)
    {
        const double g = ahead(tt, alongN);
        if (g <= 0.0)
        {
            double lo = tt - 300.0, hi = tt;
            for (int i = 0; i < 12; ++i)
            {
                const double mid = 0.5 * (lo + hi);
                (ahead(mid, alongN) > 0.0 ? lo : hi) = mid;
            }
            ahead(hi, alongN);
            return std::abs(alongN) <= 1.0 ? hi : 0.0;
        }
        g0 = g;
    }
    return 0.0;
}

// A minute before its touchdown at 1x, on the ground 6 km on its inflow side, looking at it.
bool SatelliteSim::weatherWatchTornado(uint64_t id, std::string &msg)
{
    const wx::Storm *st = stormCatalog ? stormCatalog->find(id) : nullptr;
    const double now = (double)simDayJ2000 * 86400.0 + simSecInDay;
    TornadoNow T;
    if (!st || !tornadoOf(*st, now, T) || T.spout)
    {
        msg = tx(Txt::ValThatStormPutsDownNoTornado);
        return false;
    }
    return weatherWatchTornadoOf(*st, 0, msg);
}

// A storm's tornado (or waterspout), from a copy of the storm (the Weather window's featured slots find them ahead on
// their own catalog): mode 0 a minute before touchdown, 6 km on its inflow side on the ground; mode 1 a third of the way
// into it, 8 km away and 300 m up, 8 deg up (the harness `storms tornado` framing).
bool SatelliteSim::weatherWatchTornadoOf(const wx::Storm &stIn, int mode, std::string &msg)
{
    FPROF_SCOPE("weatherWatchTornadoOf");
    const wx::Storm stCopy = stIn;
    const wx::Storm *st = &stCopy;
    const double now = (double)simDayJ2000 * 86400.0 + simSecInDay;
    TornadoNow T;
    if (!tornadoOf(*st, now, T))
    {
        msg = tx(Txt::ValThatStormPutsDownNoTornado);
        return false;
    }
    // mode 0 WATCH (the Weather window's one button, 2026-10-24: the user, "a single button that takes them to a tornado in
    // frame"; "1 minute before touchdown" put playtesters where nothing was happening yet): the funnel on the ground NOW
    // if it is, else a third of the way into its life; on the ground ~4.5 km off, looking at it. mode 1 (harness): a third
    // of the way in, 8 km away and 300 m up. mode 2 (harness `weather tornado ... act=2`): a minute before touchdown.
    const bool close = mode == 1, before = mode == 2;
    const bool onNow = now >= T.t0 + 30.0 && now <= T.t1 - 300.0;
    const double t = before ? T.t0 - 60.0 : (mode == 0 && onNow) ? now : T.t0 + (close ? 0.35 : 0.3) * (T.t1 - T.t0);
    const double dInit = close ? 8000.0 : before ? 6000.0 : T.spout ? 3000.0 : 3500.0;
    const float aglInit = close ? 300.0f : before ? 60.0f : 40.0f;
    const float elInit = close ? 8.0f : before ? 10.0f : 8.0f;
    simDayJ2000 = (int64_t)std::floor(t / 86400.0);
    simSecInDay = t - (double)simDayJ2000 * 86400.0;
    timePaused = false;
    timeScaleIdx = 0;
    timeDir = 1.0f;
    const double tw = before ? T.t0 + 30.0 : t;
    tornadoOf(*st, tw, T);   // where it is (touches down)
    const glm::dvec3 te = weatherMapToEarth(T.mapDir, tw);
    glm::dvec3 up = weatherMapToEarth(T.upwindMap, tw);
    up = glm::normalize(up - te * glm::dot(up, te));
    // Where to stand: the inflow side is the default, but a neighbouring tower's rain core or the forward flank's rain
    // under an anvil often stood there (the mature view at 2037-07-13 was in a downpour, rain at the eye 1.0). Try 12
    // bearings x 3 distances round the funnel and take the driest, from this moment's towers and shields near it (CPU:
    // a tower's rain core ~1.2 radii, a shield's area x its strength; the map's own rain is suppressed in storm
    // envelopes), with a small preference for the inflow side.
    glm::dvec3 oe = glm::normalize(te * std::cos(dInit / kRe) + up * std::sin(dInit / kRe));
    if (stormCatalog)
    {
        const double ph = (double)cloudDriftRate * (tw - kCloudDriftEpochS) + cloudDriftPhaseOffset;
        const double cph = std::cos(ph), sph = std::sin(ph);
        auto toMap = [&](const glm::dvec3 &E) { return glm::dvec3(E.x * cph - E.y * sph, E.x * sph + E.y * cph, E.z); };
        const glm::dvec3 tm = toMap(te);
        std::vector<const wx::Storm *> all, nearS;
        stormCatalog->stormsAt(tw, all);
        for (const wx::Storm *S : all)
            if (glm::dot(S->centreAt(tw), tm) > std::cos(300000.0 / kRe))
                nearS.push_back(S);
        std::vector<wx::Tower> tw3;
        std::vector<wx::Shield> sh3;
        stormCatalog->towersAt(tw, nearS, tw3);
        wx::ShieldParams sp;
        sp.headRadiusM = (float)(std::clamp((double)cv2CbRadiusKm, 1.0, 20.0) * 1000.0 * std::max((double)cv2CbFlare, 1.0) * 1.15);
        sp.headDriftM = (float)(std::clamp((double)cv2CbHeadDriftKm, 0.0, 30.0) * 1000.0);
        stormCatalog->shieldsAt(tw, nearS, sp, sh3);
        const double rTow = std::clamp((double)cv2CbRadiusKm, 1.0, 20.0) * 1000.0;
        auto wet = [&](const glm::dvec3 &E) {
            const glm::dvec3 P = toMap(E);
            double w = 0.0;
            for (const wx::Tower &T3 : tw3)
            {
                const double d = std::acos(std::clamp(glm::dot(P, T3.dir), -1.0, 1.0)) * kRe;
                const double r = rTow * (0.7 + 0.6 * (double)T3.size) * 1.2;
                w += (double)T3.strength * (0.4 + 0.6 * (double)T3.role) * std::exp(-(d * d) / (r * r));
            }
            for (const wx::Shield &S3 : sh3)
            {
                const glm::dvec3 ab = S3.b - S3.a;
                const double L2 = glm::dot(ab, ab);
                const double u = L2 > 1e-18 ? std::clamp(glm::dot(P - S3.a, ab) / L2, 0.0, 1.0) : 0.0;
                const double d = glm::length(P - (S3.a + ab * u)) * kRe;
                const double r = (double)S3.rA + ((double)S3.rB - (double)S3.rA) * u;
                if (r > 0.0 && d < r)
                    w += 0.5 * (double)S3.strength * (1.0 - (double)S3.age01) * (0.5 + (double)S3.rain) * (1.0 - d / r);
            }
            return w;
        };
        const glm::dvec3 side = glm::cross(te, up);
        double bestS = 1e30;
        for (int k = 0; k < 12; ++k)
        {
            const double a = glm::radians(30.0 * k);
            const glm::dvec3 dir = up * std::cos(a) + side * std::sin(a);
            for (int j = 0; j < 3; ++j)
            {
                const double dm = dInit + (mode == 0 ? 800.0 : 2000.0) * j;
                const glm::dvec3 E = glm::normalize(te * std::cos(dm / kRe) + dir * std::sin(dm / kRe));
                const double sc = wet(E) + 0.08 * (1.0 - std::cos(a)) + 0.02 * j;
                if (sc < bestS)
                {
                    bestS = sc;
                    oe = E;
                }
            }
        }
    }
    if (followActive)
        stopFollow();
    stopTrack();
    setObserverLatLon(glm::degrees(std::asin(std::clamp(oe.z, -1.0, 1.0))), glm::degrees(std::atan2(oe.y, oe.x)));
    obsHeightOffset = std::max(0.0f, cpuTerrainHeightM(obsLatDeg, obsLonDeg)) + aglInit;
    const glm::dvec3 oEast = glm::normalize(glm::cross(glm::dvec3(0, 0, 1), oe) + glm::dvec3(1e-12, 0, 0)), oNorth = glm::cross(oe, oEast);
    const glm::dvec3 to3 = te - oe * glm::dot(te, oe);
    const double az = glm::degrees(std::atan2(glm::dot(to3, oEast), glm::dot(to3, oNorth)));
    camera.fovYDeg = mode == 0 ? 50.0f : std::max(camera.fovYDeg, 60.0f);   // (Watch: the funnel larger in the frame)
    aimCameraAzEl((float)az, elInit);
    for (double &bb : cv2WxBakedT) bb = -1e30;   // (the weather cube re-bakes with the storm stamped)
    updatePositions(t);
    trailClearPending = true;
    skyTaaHistValid = false;
    cv2HistoryValid = false;
    wxRowsWall = -1e300;
    wxTornFix = {};
    wxTornFix.stage = 1;
    wxTornFix.te = te;
    wxTornFix.up = up;
    wxTornFix.close = close;
    wxTornFix.aglM = aglInit;
    wxTornFix.elDeg = elInit;
    wxTornFix.d0 = close ? 6000.0 : T.spout ? 2500.0 : 3000.0;
    wxTornFix.dStep = close ? 2000.0 : T.spout ? 800.0 : 1000.0;
    wxTornFix.losMax = 0.8;
    wxTornFix.rangeIdx = 0;
    char sb[200];
    const char *what = T.spout ? "waterspout" : "tornado";
    const char *side = compass(az + 180.0);
    const double dKm = std::acos(std::clamp(glm::dot(oe, te), -1.0, 1.0)) * kRe / 1000.0;
    if (mode == 0)
        snprintf(sb, sizeof(sb), tx(Txt::FmtAOnTheGroundMinLeft), what, (T.t1 - t) / 60.0,
                 std::abs(wwLat(te)), wwLat(te) >= 0.0 ? "N" : "S", std::abs(wwLon(te)), wwLon(te) >= 0.0 ? "E" : "W");
    else if (close)
        snprintf(sb, sizeof(sb), tx(Txt::FmtAThirdOfTheWayThrough), what,
                 (T.t1 - T.t0) / 60.0, dKm, side);
    else
        snprintf(sb, sizeof(sb), tx(Txt::FmtAMinuteBeforeItsTouchesDown), what,
                 (T.t1 - T.t0) / 60.0, dKm, side);
    msg = sb;
    return true;
}

// The probe frame's rain at an Earth-fixed direction (its 3x3 cells' mean; -1 off the frame).
float SatelliteSim::weatherProbeRainAt(const glm::dvec3 &e) const
{
    const WxRadarFrame &f = wxProbeFrame;
    if (!f.valid || f.cellM <= 0.0f)
        return -1.0f;
    const glm::dvec3 o = f.obs;
    const glm::dvec3 east = glm::normalize(glm::cross(glm::dvec3(0, 0, 1), o) + glm::dvec3(1e-12, 0, 0)), north = glm::cross(o, east);
    const double a = std::acos(std::clamp(glm::dot(o, e), -1.0, 1.0)) * kRe;
    glm::dvec2 u(glm::dot(e, east), glm::dot(e, north));
    const double ul = glm::length(u);
    u = ul > 1e-12 ? u / ul : glm::dvec2(0.0);
    const double fx = u.x * a / f.cellM + 0.5 * kWxN - 0.5, fy = 0.5 * kWxN - u.y * a / f.cellM - 0.5;
    const int ci = (int)std::lround(fx), cj = (int)std::lround(fy);
    float acc = 0.0f;
    int n = 0;
    for (int j = cj - 1; j <= cj + 1; ++j)
        for (int i = ci - 1; i <= ci + 1; ++i)
            if (i >= 0 && j >= 0 && i < kWxN && j < kWxN)
            {
                acc += f.rain[(size_t)j * kWxN + i];
                ++n;
            }
    return n ? acc / (float)n : -1.0f;
}

// After a tornado Go to (weatherWatchTornadoOf): wait for the weather cube to re-bake round the new place, take one radar
// probe, and move the eye to the driest of 48 spots round the funnel (rain at the spot, and along the line of sight to
// the funnel, which a rain-wrapped funnel hides behind), keeping the inflow side when it is as dry.
void SatelliteSim::weatherTornadoFixTick()
{
    FPROF_SCOPE("weatherTornadoFixTick");
    WxTornFix &F = wxTornFix;
    if (F.stage == 0)
        return;
    if (F.stage == 1)
    {
        if (++F.frames >= 10)
        {
            F.stage = 2;
            F.frames = 0;
        }
        return;
    }
    if (F.stage == 2)
    {
        if (!wxRadarInFlight)
        {
            wxProbeFrame.valid = false;
            wxRadarProbe = true;
            wxRadarProbeRange = F.rangeIdx;
            wxRadarWanted = true;
            F.stage = 3;
        }
        return;
    }
    // stage 3: the probe dispatched this frame or earlier; read it back once it is in.
    if (wxRadarInFlight && wxRadarDispProbe)
        weatherRadarReadback();
    if (!wxProbeFrame.valid)
    {
        if (++F.frames > 30)
            F.stage = 0;   // (no clouds pass: give up)
        return;
    }
    F.stage = 0;
    const glm::dvec3 te = F.te, up = F.up;
    const glm::dvec3 side = glm::cross(te, up);
    auto rainOr = [&](const glm::dvec3 &E) { const float r = weatherProbeRainAt(E); return r < 0.0f ? 2.0 : (double)r; };
    auto score = [&](const glm::dvec3 &E, double a) {
        const double r = rainOr(E);
        double los = 0.0;
        for (int k = 1; k <= 7; ++k)
        {
            const double w = F.losMax * k / 7.0;
            los += rainOr(glm::normalize(E * (1.0 - w) + te * w));
        }
        return 3.0 * r + 1.0 * los / 7.0 + 0.04 * (1.0 - std::cos(a));
    };
    const glm::dvec3 oe0 = glm::normalize(glm::dvec3(obsDir));
    F.before = std::max(0.0f, weatherProbeRainAt(oe0));
    // (the first guess is a candidate too: in a downpour everywhere, the rings moved a waterspout view INTO heavier rain)
    const double j0 = std::max(0.0, (std::acos(std::clamp(glm::dot(oe0, te), -1.0, 1.0)) * kRe - F.d0) / std::max(F.dStep, 1.0));
    double bestS = score(oe0, std::acos(std::clamp(glm::dot(glm::normalize(oe0 - te * glm::dot(oe0, te)), up), -1.0, 1.0))) + 0.05 * j0;
    glm::dvec3 best = oe0;
    const double d0 = F.d0;
    for (int k = 0; k < 12; ++k)
    {
        const double a = glm::radians(30.0 * k);
        const glm::dvec3 dir = up * std::cos(a) + side * std::sin(a);
        for (int j = 0; j < 4; ++j)
        {
            const double dm = d0 + F.dStep * j;
            const glm::dvec3 E = glm::normalize(te * std::cos(dm / kRe) + dir * std::sin(dm / kRe));
            const double sc = score(E, a) + 0.05 * j;
            if (sc < bestS)
            {
                bestS = sc;
                best = E;
            }
        }
    }
    F.after = std::max(0.0f, weatherProbeRainAt(best));
    F.distKm = std::acos(std::clamp(glm::dot(best, te), -1.0, 1.0)) * kRe / 1000.0;
    if (std::acos(std::clamp(glm::dot(best, oe0), -1.0, 1.0)) * kRe < 300.0)
        return;
    setObserverLatLon(glm::degrees(std::asin(std::clamp(best.z, -1.0, 1.0))), glm::degrees(std::atan2(best.y, best.x)));
    obsHeightOffset = std::max(0.0f, cpuTerrainHeightM(obsLatDeg, obsLonDeg)) + F.aglM;
    const glm::dvec3 oEast = glm::normalize(glm::cross(glm::dvec3(0, 0, 1), best) + glm::dvec3(1e-12, 0, 0)), oNorth = glm::cross(best, oEast);
    const glm::dvec3 to3 = te - best * glm::dot(te, best);
    aimCameraAzEl((float)glm::degrees(std::atan2(glm::dot(to3, oEast), glm::dot(to3, oNorth))), F.elDeg);
    updatePositions((double)simDayJ2000 * 86400.0 + simSecInDay);
    trailClearPending = true;
    skyTaaHistValid = false;
    cv2HistoryValid = false;
    char b[160];
    snprintf(b, sizeof(b), "weather: %s view moved out of the rain (%.2f -> %.2f), %.1f km from it", F.what, F.before, F.after, F.distKm);
    Log::lineDeferred(b);
}

// 20 minutes before the line reaches you, where you stand, at 1x, looking at it (into its motion).
bool SatelliteSim::weatherWatchPassage(uint64_t id, std::string &msg)
{
    const wx::Storm *st = stormCatalog ? stormCatalog->find(id) : nullptr;
    const double now = (double)simDayJ2000 * 86400.0 + simSecInDay;
    const double tp = st ? weatherLinePassage(*st, now) : 0.0;
    if (tp <= 0.0)
    {
        msg = tx(Txt::ValItsLineDoesNotPassOver);
        return false;
    }
    const double t = std::max(now, tp - 1200.0);
    simDayJ2000 = (int64_t)std::floor(t / 86400.0);
    simSecInDay = t - (double)simDayJ2000 * 86400.0;
    timePaused = false;
    timeScaleIdx = 0;
    timeDir = 1.0f;
    // Look along the line's motion, backwards: toward it as it comes.
    const glm::dvec3 c0 = weatherMapToEarth(st->centreAt(t), t), c1 = weatherMapToEarth(st->centreAt(t + 600.0), t + 600.0);
    const glm::dvec3 oe = glm::normalize(glm::dvec3(obsDir));
    const glm::dvec3 oEast = glm::normalize(glm::cross(glm::dvec3(0, 0, 1), oe) + glm::dvec3(1e-12, 0, 0)), oNorth = glm::cross(oe, oEast);
    const glm::dvec3 mv = c1 - c0;
    const double az = glm::degrees(std::atan2(-glm::dot(mv, oEast), -glm::dot(mv, oNorth)));
    if (followActive)
        stopFollow();
    stopTrack();
    camera.fovYDeg = std::max(camera.fovYDeg, 60.0f);
    aimCameraAzEl((float)az, 6.0f);
    updatePositions(t);
    trailClearPending = true;
    skyTaaHistValid = false;
    cv2HistoryValid = false;
    wxRowsWall = -1e300;
    msg = std::string(tx(Txt::TxtMinutesBeforeTheLineReaches)) + compass(az) + tx(Txt::ValIntoIt);
    return true;
}

// Go to: as the harness's `storms go` — on the ground (1.5 km above sea level, floored at the ground) ~50 km upwind of
// the storm's strongest tower (the anvil blows away from you), looking at it a few degrees up.
bool SatelliteSim::weatherGoToStorm(uint64_t id, std::string &msg, double distM, float aglM, float elDeg)
{
    FPROF_SCOPE("weatherGoToStorm");
    if (!stormCatalog)
    {
        msg = tx(Txt::ValNoStormCatalogYet);
        return false;
    }
    const double t = (double)simDayJ2000 * 86400.0 + simSecInDay;
    std::vector<wx::Tower> tw;
    stormCatalog->towersAt(t, tw);
    const wx::Tower *best = nullptr;
    for (const wx::Tower &w : tw)
        if (w.stormId == id && (!best || w.strength > best->strength))
            best = &w;
    WxStormRow row;
    const bool alive = weatherStormNow(id, t, row);
    if (!best && !alive)
    {
        msg = tx(Txt::ValThatStormIsNotAliveNow);
        return false;
    }
    const glm::dvec3 te = best ? weatherMapToEarth(best->dir, t) : row.e;
    const glm::dvec3 east = glm::normalize(glm::cross(glm::dvec3(0, 0, 1), te) + glm::dvec3(1e-12, 0, 0));
    const glm::dvec3 north = glm::cross(te, east);
    double b = 0.0;   // the bearing from the tower to stand at: upwind (against the anvil's shear)
    if (best)
    {
        const glm::dvec3 d = glm::normalize(best->dir);
        const glm::dvec3 me = glm::normalize(glm::cross(glm::dvec3(0, 0, 1), d) + glm::dvec3(1e-12, 0, 0));
        const glm::dvec3 mn = glm::cross(d, me);
        const glm::dvec3 sh(best->shear);
        b = std::atan2(glm::dot(sh, me), glm::dot(sh, mn)) + glm::pi<double>();
    }
    const double ang = distM / kRe;
    const glm::dvec3 oe = glm::normalize(te * std::cos(ang) + (east * std::sin(b) + north * std::cos(b)) * std::sin(ang));
    if (followActive)
        stopFollow();
    stopTrack();
    setObserverLatLon(glm::degrees(std::asin(std::clamp(oe.z, -1.0, 1.0))), glm::degrees(std::atan2(oe.y, oe.x)));
    obsHeightOffset = aglM < 0.0f ? 1500.0f : std::max(0.0f, cpuTerrainHeightM(obsLatDeg, obsLonDeg)) + aglM;
    const glm::dvec3 oEast = glm::normalize(glm::cross(glm::dvec3(0, 0, 1), oe) + glm::dvec3(1e-12, 0, 0)), oNorth = glm::cross(oe, oEast);
    const glm::dvec3 to3 = te - oe * glm::dot(te, oe);
    const double az = glm::degrees(std::atan2(glm::dot(to3, oEast), glm::dot(to3, oNorth)));
    camera.fovYDeg = std::max(camera.fovYDeg, 60.0f);
    aimCameraAzEl((float)az, elDeg);
    wxStormLastTE = te;
    wxStormLastUp = glm::normalize(oe - te * glm::dot(oe, te));
    updatePositions(t);
    trailClearPending = true;
    skyTaaHistValid = false;
    cv2HistoryValid = false;
    wxRowsWall = -1e300;
    char sb[200];
    snprintf(sb, sizeof(sb), tx(Txt::FmtYouAreKmOfItLooking), kindName(alive ? row.kind : (int)best->kind),
             alive ? stageName(row.stage) : "alive", distM / 1000.0, compass(glm::degrees(b)), compass(az));
    msg = sb;
    return true;
}

// WATCH (2026-10-24, the Weather window's one button for a storm): its tornado if one is on the ground now; else the storm
// at its mature moment (the clock moved forward to it while it is still growing, never back), from the ground ~35 km
// upwind of its strongest tower, then moved out of the rain by the radar probe (weatherTornadoFixTick).
bool SatelliteSim::weatherWatchStormNow(uint64_t id, std::string &msg)
{
    FPROF_SCOPE("weatherWatchStormNow");
    const double now = (double)simDayJ2000 * 86400.0 + simSecInDay;
    WxStormRow row;
    if (!weatherStormNow(id, now, row))
    {
        msg = tx(Txt::ValThatStormHasGone);
        return false;
    }
    const wx::Storm *st = stormCatalog ? stormCatalog->find(id) : nullptr;
    if (st && row.tTorn0 > 0.0 && now >= row.tTorn0 + 30.0 && now <= row.tTorn1 - 180.0)
        return weatherWatchTornadoOf(*st, 0, msg);
    const double t = (row.tMature > now && row.tMature < row.tEnd - 600.0) ? row.tMature : now;
    const double days = std::floor(t / 86400.0);
    simDayJ2000 = (int64_t)days;
    simSecInDay = t - days * 86400.0;
    timePaused = false;
    timeScaleIdx = 0;
    timeDir = 1.0f;
    const bool line = row.kind == (int)wx::StormKind::SquallLine || row.kind == (int)wx::StormKind::Mcs;
    const double dist = line ? 30000.0 : 35000.0;
    std::string m2;
    if (!weatherGoToStorm(id, m2, dist, 300.0f, 10.0f))
    {
        msg = m2;
        return false;
    }
    for (double &bb : cv2WxBakedT) bb = -1e30;
    wxTornFix = {};
    wxTornFix.stage = 1;
    wxTornFix.te = wxStormLastTE;
    wxTornFix.up = wxStormLastUp;
    wxTornFix.aglM = 300.0f;
    wxTornFix.elDeg = 10.0f;
    wxTornFix.d0 = dist - 10000.0;
    wxTornFix.dStep = 7000.0;
    wxTornFix.rangeIdx = 1;
    wxTornFix.losMax = 0.6;
    wxTornFix.what = kindName(row.kind);
    char sb[220];
    snprintf(sb, sizeof(sb), tx(Txt::FmtSeenFromAboutKmAway), t > now + 60.0 ? tx(Txt::TxtAtItsHeight) : "",
             kindName(row.kind), std::abs(wwLat(row.e)), wwLat(row.e) >= 0.0 ? "N" : "S", std::abs(wwLon(row.e)),
             wwLon(row.e) >= 0.0 ? "E" : "W", dist / 1000.0);
    msg = sb;
    return true;
}

// Watch (harness `weather watch`): the clock to the storm's mature stage (its dominant tower grown, the anvil spread) at 1x, then Go to.
bool SatelliteSim::weatherWatchStorm(uint64_t id, std::string &msg)
{
    const double now = (double)simDayJ2000 * 86400.0 + simSecInDay;
    WxStormRow row;
    if (!weatherStormNow(id, now, row))
    {
        msg = tx(Txt::ValThatStormIsNotAliveNow);
        return false;
    }
    const double t = row.tMature;
    const double days = std::floor(t / 86400.0);
    simDayJ2000 = (int64_t)days;
    simSecInDay = t - days * 86400.0;
    timePaused = false;
    timeScaleIdx = 0;
    timeDir = 1.0f;
    std::string m2;
    const bool ok = weatherGoToStorm(id, m2);
    char sb[240];
    snprintf(sb, sizeof(sb), "%s %s: %s", t >= now ? tx(Txt::TxtAheadTo) : tx(Txt::TxtBackTo),
             t >= now ? tx(Txt::TxtItsMatureStage) : tx(Txt::TxtItsMatureStage), ok ? m2.c_str() : tx(Txt::TxtItHasGone));
    msg = sb;
    return ok;
}

// ── The radar: the GPU pass ─────────────────────────────────────────────────────────────────────────────────────
void SatelliteSim::weatherRadarEnsure(VulkanContext &ctx)
{
    if (wxRadarPipe || wxRadarFailed)
        return;
    if (!cv2MarchDescLayout || !cv2MarchDescSet)
    {
        wxRadarFailed = true;
        Log::line("weather radar: no cloud march descriptor set (clouds v2 not built)");
        return;
    }
    try
    {
        VkDevice dev = ctx.device;
        VkDescriptorSetLayoutBinding b{0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
        VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        li.bindingCount = 1;
        li.pBindings = &b;
        vkCreateDescriptorSetLayout(dev, &li, nullptr, &wxRadarSetLayout);
        VkDescriptorSetLayout lays[2] = {cv2MarchDescLayout, wxRadarSetLayout};
        VkPushConstantRange pr{VK_SHADER_STAGE_COMPUTE_BIT, 0, 32};
        VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pli.setLayoutCount = 2;
        pli.pSetLayouts = lays;
        pli.pushConstantRangeCount = 1;
        pli.pPushConstantRanges = &pr;
        vkCreatePipelineLayout(dev, &pli, nullptr, &wxRadarPipeLayout);
        VkShaderModule mod = ctx.loadShader("shaders/weather_radar.comp.spv");
        VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        ci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, mod, "main", nullptr};
        ci.layout = wxRadarPipeLayout;
        const VkResult res = vkCreateComputePipelines(dev, ctx.pipelineCache, 1, &ci, nullptr, &wxRadarPipe);
        vkDestroyShaderModule(dev, mod, nullptr);
        if (res != VK_SUCCESS)
            throw std::runtime_error("vkCreateComputePipelines");
        const VkDeviceSize bytes = (VkDeviceSize)kWxN * kWxN * 16;
        ctx.createBuffer(bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT,
                         wxRadarBuf, wxRadarMem);
        vkMapMemory(dev, wxRadarMem, 0, bytes, 0, &wxRadarMapped);
        VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1};
        VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        pi.maxSets = 1;
        pi.poolSizeCount = 1;
        pi.pPoolSizes = &ps;
        vkCreateDescriptorPool(dev, &pi, nullptr, &wxRadarPool);
        VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        ai.descriptorPool = wxRadarPool;
        ai.descriptorSetCount = 1;
        ai.pSetLayouts = &wxRadarSetLayout;
        vkAllocateDescriptorSets(dev, &ai, &wxRadarSet);
        VkDescriptorBufferInfo bi{wxRadarBuf, 0, bytes};
        VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.dstSet = wxRadarSet;
        w.dstBinding = 0;
        w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        w.pBufferInfo = &bi;
        frameSafeUpdateDescriptorSets(dev, 1, &w, 0, nullptr);
        if (ctx.timestampPeriodNs > 0.0)
        {
            VkQueryPoolCreateInfo qi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
            qi.queryType = VK_QUERY_TYPE_TIMESTAMP;
            qi.queryCount = 2;
            if (vkCreateQueryPool(dev, &qi, nullptr, &wxRadarQuery) != VK_SUCCESS)
                wxRadarQuery = VK_NULL_HANDLE;
        }
        Log::line("weather radar: pipeline ready");
    }
    catch (const std::exception &e)
    {
        Log::line(std::string("weather radar: ") + e.what());
        wxRadarFailed = true;
    }
}

void SatelliteSim::weatherRadarRecord(VkCommandBuffer cmd)
{
    wxRadarRanCapable = true;
    if (!wxRadarWanted || !ctx_)
        return;
    wxRadarWanted = false;
    FPROF_SCOPE("weatherRadarRecord");
    VulkanContext &ctx = *ctx_;
    weatherRadarEnsure(ctx);
    if (!wxRadarPipe)
        return;
    // The cloud passes' writes (the weather cube's bake, the storm stamp) before this read.
    VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &mb, 0,
                         nullptr, 0, nullptr);
    if (wxRadarQuery)
    {
        vkCmdResetQueryPool(cmd, wxRadarQuery, 0, 2);
        vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, wxRadarQuery, 0);
    }
    const float cellM = wxRadarProbe ? 2000.0f * kWxRangesKm[std::clamp(wxRadarProbeRange, 0, 2)] / (float)kWxN
                                     : 2000.0f * kWxRangesKm[std::clamp(wxRangeIdx, 0, 2)] / (float)kWxN;
    wxRadarDispProbe = wxRadarProbe;
    wxRadarProbe = false;
    struct
    {
        glm::vec4 obs, grid;
    } pc{glm::vec4(glm::normalize(obsDir), cellM), glm::vec4((float)kWxN, 0.0f, (float)kWxN, kWxSampleAglM)};
    VkDescriptorSet sets[2] = {cv2MarchDescSet, wxRadarSet};
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, wxRadarPipe);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, wxRadarPipeLayout, 0, 2, sets, 0, nullptr);
    vkCmdPushConstants(cmd, wxRadarPipeLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
    vkCmdDispatch(cmd, kWxN / 8, kWxN / 8, 1);
    VkMemoryBarrier hb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    hb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    hb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &hb, 0, nullptr, 0, nullptr);
    if (wxRadarQuery)
        vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, wxRadarQuery, 1);
    wxRadarInFlight = true;
    wxRadarFrame = ctx_ ? ctx_->frameIndex : 0; // read back once this frame has finished (frames in flight)
    wxRadarDispWall = wallNow();
    wxRadarDispSim = (double)simDayJ2000 * 86400.0 + simSecInDay;
    wxRadarDispObs = glm::normalize(glm::dvec3(obsDir));
    wxRadarDispCellM = cellM;
    ++wxRadarDispatches;
}

void SatelliteSim::weatherRadarDestroy(VkDevice device)
{
    if (wxRadarMapped)
        vkUnmapMemory(device, wxRadarMem);
    wxRadarMapped = nullptr;
    if (wxRadarBuf) vkDestroyBuffer(device, wxRadarBuf, nullptr);
    if (wxRadarMem) vkFreeMemory(device, wxRadarMem, nullptr);
    if (wxRadarPipe) vkDestroyPipeline(device, wxRadarPipe, nullptr);
    if (wxRadarPipeLayout) vkDestroyPipelineLayout(device, wxRadarPipeLayout, nullptr);
    if (wxRadarPool) vkDestroyDescriptorPool(device, wxRadarPool, nullptr);
    if (wxRadarSetLayout) vkDestroyDescriptorSetLayout(device, wxRadarSetLayout, nullptr);
    if (wxRadarQuery) vkDestroyQueryPool(device, wxRadarQuery, nullptr);
    wxRadarBuf = VK_NULL_HANDLE;
    wxRadarMem = VK_NULL_HANDLE;
    wxRadarPipe = VK_NULL_HANDLE;
    wxRadarPipeLayout = VK_NULL_HANDLE;
    wxRadarPool = VK_NULL_HANDLE;
    wxRadarSetLayout = VK_NULL_HANDLE;
    wxRadarQuery = VK_NULL_HANDLE;
    std::free(wxStageMapped);
    wxStageMapped = nullptr;
    if (wxStage) vkDestroyBuffer(device, wxStage, nullptr);
    if (wxStageMem) vkFreeMemory(device, wxStageMem, nullptr);
    if (wxImgSampler) vkDestroySampler(device, wxImgSampler, nullptr);
    if (wxImgView) vkDestroyImageView(device, wxImgView, nullptr);
    if (wxImg) vkDestroyImage(device, wxImg, nullptr);
    if (wxImgMem) vkFreeMemory(device, wxImgMem, nullptr);
    wxStage = VK_NULL_HANDLE;
    wxStageMem = VK_NULL_HANDLE;
    wxImgSampler = VK_NULL_HANDLE;
    wxImgView = VK_NULL_HANDLE;
    wxImg = VK_NULL_HANDLE;
    wxImgMem = VK_NULL_HANDLE;
    std::free(wxMapStageMapped);
    wxMapStageMapped = nullptr;
    if (wxMapStage) vkDestroyBuffer(device, wxMapStage, nullptr);
    if (wxMapStageMem) vkFreeMemory(device, wxMapStageMem, nullptr);
    if (wxMapSampler) vkDestroySampler(device, wxMapSampler, nullptr);
    if (wxMapView) vkDestroyImageView(device, wxMapView, nullptr);
    if (wxMapImg) vkDestroyImage(device, wxMapImg, nullptr);
    if (wxMapImgMem) vkFreeMemory(device, wxMapImgMem, nullptr);
    wxMapStage = VK_NULL_HANDLE;
    wxMapStageMem = VK_NULL_HANDLE;
    wxMapSampler = VK_NULL_HANDLE;
    wxMapView = VK_NULL_HANDLE;
    wxMapImg = VK_NULL_HANDLE;
    wxMapImgMem = VK_NULL_HANDLE;
}

// Last frame's dispatch, read back (the fence has been waited on: single frame in flight).
void SatelliteSim::weatherRadarReadback()
{
    if (!wxRadarInFlight || !wxRadarMapped || (ctx_ && ctx_->completedFrame() < wxRadarFrame))
        return;
    wxRadarInFlight = false;
    FPROF_SCOPE("weatherRadarReadback");
    const auto tCpu0 = std::chrono::steady_clock::now();
    if (wxRadarQuery && ctx_)
    {
        uint64_t ts[2] = {0, 0};
        if (vkGetQueryPoolResults(ctx_->device, wxRadarQuery, 0, 2, sizeof(ts), ts, sizeof(uint64_t), VK_QUERY_RESULT_64_BIT) == VK_SUCCESS)
            wxRadarGpuMs = (float)((double)(ts[1] - ts[0]) * ctx_->timestampPeriodNs * 1e-6);
    }
    VkMappedMemoryRange rg{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
    rg.memory = wxRadarMem;
    rg.size = VK_WHOLE_SIZE;
    if (ctx_)
        vkInvalidateMappedMemoryRanges(ctx_->device, 1, &rg);
    const size_t n = (size_t)kWxN * kWxN;
    WxRadarFrame f;
    f.rain.resize(n);
    f.cover.resize(n);
    f.ground.resize(n);
    f.core.resize(n);
    const float *src = static_cast<const float *>(wxRadarMapped);
    for (size_t i = 0; i < n; ++i)
    {
        f.rain[i] = src[i * 4 + 0];
        f.cover[i] = src[i * 4 + 1];
        f.ground[i] = src[i * 4 + 2];
        f.core[i] = src[i * 4 + 3];
    }
    f.simT = wxRadarDispSim;
    f.obs = wxRadarDispObs;
    f.cellM = wxRadarDispCellM;
    f.valid = true;
    if (wxRadarDispProbe)
    {
        wxRadarDispProbe = false;
        wxProbeFrame = std::move(f);
        return;
    }
    // The loop keeps a frame every kWxLoopStepS of sim time at one place and range (anything else starts it again).
    if (!wxLoopFrames.empty())
    {
        const WxRadarFrame &b = wxLoopFrames.back();
        const double moved = std::acos(std::clamp(glm::dot(b.obs, f.obs), -1.0, 1.0)) * kRe;
        if (b.cellM != f.cellM || moved > 2.0 * f.cellM || f.simT < b.simT - 1.0)
            wxLoopFrames.clear();
    }
    if (wxLoopFrames.empty() || f.simT - wxLoopFrames.back().simT >= kWxLoopStepS)
    {
        WxRadarFrame lf;
        lf.rain = f.rain;
        lf.cover = f.cover;
        lf.simT = f.simT;
        lf.obs = f.obs;
        lf.cellM = f.cellM;
        lf.valid = true;
        wxLoopFrames.push_back(std::move(lf));
        while ((int)wxLoopFrames.size() > kWxLoopMax)
            wxLoopFrames.pop_front();
    }
    wxShown = std::move(f);
    // Statistics (harness `weather radar`).
    wxStats = {};
    const glm::dvec3 up = wxShown.obs;
    const double lat = std::asin(std::clamp(up.z, -1.0, 1.0));
    const double sunDec = std::asin(std::clamp((double)glm::normalize(sunDirECI).z, -1.0, 1.0));
    float maxR = 0.0f;
    int rain = 0, snow = 0, core = 0;
    const float rrMin = rainForDbz(kDbzMin);
    const float R = (float)kWxRangesKm[std::clamp(wxRangeIdx, 0, 2)];
    for (int j = 0; j < kWxN; ++j)
        for (int i = 0; i < kWxN; ++i)
        {
            const size_t k = (size_t)j * kWxN + i;
            const float rr = wxShown.rain[k];
            maxR = std::max(maxR, rr);
            if (rr >= rrMin)
            {
                ++rain;
                if (wxShown.core[k] > 0.05f)
                    ++core;
            }
        }
    (void)lat;
    (void)sunDec;
    (void)snow;
    (void)R;
    const int c = kWxN / 2;
    wxStats.centreRain = 0.25f * (wxShown.rain[(size_t)(c - 1) * kWxN + c - 1] + wxShown.rain[(size_t)(c - 1) * kWxN + c] +
                                  wxShown.rain[(size_t)c * kWxN + c - 1] + wxShown.rain[(size_t)c * kWxN + c]);
    wxStats.maxRain = maxR;
    wxStats.maxDbz = maxR > 0.0f ? rainDbz(maxR) : 0.0f;
    wxStats.raining = rain;
    wxStats.rainShare = (float)rain / (float)(kWxN * kWxN);
    wxStats.coreShare = rain > 0 ? (float)core / (float)rain : 0.0f;
    if (!(wxLoop && wxLoopFrames.size() >= 2))   // (the loop rasters its own frames)
        weatherRadarRaster(wxShown);
    wxRadarCpuMs = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - tCpu0).count();
}

// ── The radar: the image ────────────────────────────────────────────────────────────────────────────────────────
bool SatelliteSim::weatherRadarEnsureImage(UIRenderer &ui)
{
    if (wxImg != VK_NULL_HANDLE)
        return wxUiId != 0;
    if (!ctx_ || wxImageFailed)
        return false;
    VulkanContext &ctx = *ctx_;
    ctx.createImage(kWxN, kWxN, VK_FORMAT_R8G8B8A8_SRGB, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, wxImg, wxImgMem);
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = wxImg;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = VK_FORMAT_R8G8B8A8_SRGB;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCreateImageView(ctx.device, &vi, nullptr, &wxImgView);
    VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    si.magFilter = si.minFilter = VK_FILTER_NEAREST; // radar cells read as cells
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    vkCreateSampler(ctx.device, &si, nullptr, &wxImgSampler);
    const VkDeviceSize bytes = (VkDeviceSize)kWxN * kWxN * 4;
    // The raster's CPU copy; each upload takes this frame's staging (ctx.stagingAlloc: frames in flight).
    wxStageMapped = std::calloc((size_t)bytes, 1);
    VkCommandBuffer cmd = ctx.beginOneTimeCommands();
    ctx.imageBarrier(cmd, wxImg, 0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                     VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    const VkClearColorValue clear{{0.02f, 0.03f, 0.05f, 1.0f}};
    const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(cmd, wxImg, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear, 1, &range);
    ctx.imageBarrier(cmd, wxImg, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    ctx.endOneTimeCommands(cmd);
    wxUiId = ui.registerImage(ctx.device, wxImgView, wxImgSampler);
    if (wxUiId == 0)
    {
        wxImageFailed = true;
        Log::line("weather radar: no free UI image slot for the radar");
    }
    return wxUiId != 0;
}

void SatelliteSim::weatherRadarRecordUpload(VkCommandBuffer cmd)
{
    if (wxMapUploadPending && wxMapImg != VK_NULL_HANDLE && ctx_)
    {
        // The cyclone map (SatelliteSimWeatherCyclones.cpp), when re-rastered.
        wxMapUploadPending = false;
        VulkanContext &c = *ctx_;
        c.imageBarrier(cmd, wxMapImg, VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        const VkDeviceSize mb = (VkDeviceSize)kWxMapW * kWxMapH * 4;
        const VulkanContext::StagingAlloc ms = c.stagingAlloc(mb);
        std::memcpy(ms.ptr, wxMapStageMapped, (size_t)mb);
        VkBufferImageCopy rg{};
        rg.bufferOffset = ms.off;
        rg.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        rg.imageExtent = {(uint32_t)kWxMapW, (uint32_t)kWxMapH, 1};
        vkCmdCopyBufferToImage(cmd, ms.buf, wxMapImg, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &rg);
        c.imageBarrier(cmd, wxMapImg, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                       VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    }
    if (!wxUploadPending || wxImg == VK_NULL_HANDLE || !ctx_)
        return;
    wxUploadPending = false;
    VulkanContext &ctx = *ctx_;
    ctx.imageBarrier(cmd, wxImg, VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    const VkDeviceSize bytes = (VkDeviceSize)kWxN * kWxN * 4;
    const VulkanContext::StagingAlloc st = ctx.stagingAlloc(bytes);
    std::memcpy(st.ptr, wxStageMapped, (size_t)bytes);
    VkBufferImageCopy region{};
    region.bufferOffset = st.off;
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {(uint32_t)kWxN, (uint32_t)kWxN, 1};
    vkCmdCopyBufferToImage(cmd, st.buf, wxImg, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    ctx.imageBarrier(cmd, wxImg, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
}

// The raster: a dark land / sea map (coasts, the ground's height), the low cloud faintly, the precipitation in NWS
// colours (snow / mixed by the air temperature 500 m over the ground there: the rain particles' climate, CLAUDE.md
// "Precipitation type"), range rings, the radar's circle.
void SatelliteSim::weatherRadarRaster(const WxRadarFrame &f)
{
    FPROF_SCOPE("weatherRadarRaster");
    if (!wxStageMapped || !f.valid || f.rain.empty())
        return;
    const int N = kWxN;
    const glm::dvec3 up = f.obs;
    const glm::dvec3 E = glm::normalize(glm::cross(glm::dvec3(0, 0, 1), up) + glm::dvec3(1e-12, 0, 0));
    const glm::dvec3 Nn = glm::cross(up, E);
    const double sunDec = std::asin(std::clamp((double)glm::normalize(sunDirECI).z, -1.0, 1.0));
    const double cellM = f.cellM;
    const double radiusKm = cellM * N * 0.5 / 1000.0;
    const double ringStepKm = radiusKm <= 60.0 ? 10.0 : radiusKm <= 200.0 ? 50.0 : 100.0;
    const double pxKm = cellM / 1000.0;
    // The land mask per cell (one lookup each), for the coasts: made again only when the centre or the range moved.
    static std::vector<uint8_t> ocean;
    static std::vector<float> latv;
    static glm::dvec3 maskObs{0.0};
    static double maskCell = -1.0;
    const bool maskOk = maskCell == cellM && glm::length(maskObs - up) * kRe < 0.05 * cellM && ocean.size() == (size_t)N * N;
    if (!maskOk)
    {
        ocean.assign((size_t)N * N, 0);
        latv.assign((size_t)N * N, 0.0f);
        maskObs = up;
        maskCell = cellM;
    }
    for (int j = 0; j < N && !maskOk; ++j)
        for (int i = 0; i < N; ++i)
        {
            const double x = ((i + 0.5) - 0.5 * N) * cellM, y = (0.5 * N - (j + 0.5)) * cellM;
            const double d = std::sqrt(x * x + y * y), a = d / kRe;
            const glm::dvec3 u = d > 0.0 ? (E * x + Nn * y) / d : E;
            const glm::dvec3 p = up * std::cos(a) + u * std::sin(a);
            const double lat = std::asin(std::clamp(p.z, -1.0, 1.0)), lon = std::atan2(p.y, p.x);
            ocean[(size_t)j * N + i] = oceanAt(lat, lon) ? 1 : 0;
            latv[(size_t)j * N + i] = (float)lat;
        }
    uint8_t *px = static_cast<uint8_t *>(wxStageMapped);
    int snow = 0;
    const float rrDraw = rainForDbz(kDbzMin - 3.0f);
    for (int j = 0; j < N; ++j)
        for (int i = 0; i < N; ++i)
        {
            const size_t k = (size_t)j * N + i;
            const bool sea = ocean[k] != 0;
            glm::vec3 col = sea ? glm::vec3(12, 22, 38) : glm::vec3(28, 31, 26);
            const float g = f.ground.empty() ? 0.0f : f.ground[k];
            if (!sea)
                col += glm::vec3(30, 26, 18) * std::clamp(g / 4000.0f, 0.0f, 1.0f);
            // The coast: a neighbour on the other side.
            bool coast = false;
            if (i > 0 && ocean[k - 1] != ocean[k]) coast = true;
            if (j > 0 && ocean[k - (size_t)N] != ocean[k]) coast = true;
            if (coast)
                col = glm::mix(col, glm::vec3(105, 115, 125), 0.6f);
            // The low cloud, faintly.
            if (wxShowClouds && !f.cover.empty())
                col = glm::mix(col, glm::vec3(150, 155, 165), 0.33f * std::clamp(f.cover[k], 0.0f, 1.0f));
            // Range rings and the cardinal lines.
            const double x = ((i + 0.5) - 0.5 * N) * pxKm, y = (0.5 * N - (j + 0.5)) * pxKm;
            const double r = std::sqrt(x * x + y * y);
            const double ringD = std::abs(r - ringStepKm * std::round(r / ringStepKm)) / pxKm;
            if (r > 0.5 * ringStepKm && r <= radiusKm + 0.5 * pxKm && ringD < 0.6)
                col = glm::mix(col, glm::vec3(120, 130, 140), 0.55f);
            else if ((std::abs(x) < 0.5 * pxKm || std::abs(y) < 0.5 * pxKm) && r > 2.5 * pxKm && r < radiusKm)
                col = glm::mix(col, glm::vec3(90, 100, 110), 0.25f);
            // The precipitation.
            const float rr = f.rain[k];
            if (rr >= rrDraw)
            {
                const float dbz = rainDbz(rr);
                const double lat = latv[k], latD = glm::degrees(lat);
                const double season = (sunDec / glm::radians(23.44)) * (latD >= 0.0 ? 1.0 : -1.0);
                const double tC = -18.0 + 45.0 * std::pow(std::cos(lat), 1.5) + std::min(0.25 * std::abs(latD), 15.0) * season
                                - 0.0065 * ((double)g + 500.0);
                glm::vec3 pc = tC < -1.0 ? ramp(kSnow, dbz) : tC < 2.5 ? ramp(kMix, dbz) : ramp(kRain, dbz);
                if (tC < -1.0)
                    ++snow;
                const float a = std::clamp((dbz - (kDbzMin - 3.0f)) / 3.0f, 0.0f, 1.0f);
                col = glm::mix(col, pc, a);
            }
            // Outside the radar's circle: dimmed.
            if (r > radiusKm)
                col *= 0.45f;
            uint8_t *o = &px[k * 4];
            o[0] = (uint8_t)std::clamp((int)std::lround(col.r), 0, 255);
            o[1] = (uint8_t)std::clamp((int)std::lround(col.g), 0, 255);
            o[2] = (uint8_t)std::clamp((int)std::lround(col.b), 0, 255);
            o[3] = 255;
        }
    if (&f == &wxShown)
        wxStats.snowing = snow;
    wxUploadPending = true;
}

// ── The window ──────────────────────────────────────────────────────────────────────────────────────────────────
void SatelliteSim::buildWeatherWindow(const UIInput &inp, UIRenderer &ui)
{
    FPROF_SCOPE("buildWeatherWindow");
    if (!wxChrome.open)
    {
        wxRadarWanted = false;
        return;
    }
    wxText_.clear(); // last frame's strings: recorded by now
    if (wxChrome.w <= 0.0f)
    {
        wxChrome.w = winDefault(600.0f, 0.42f, inp.screenW, 400.0f);
        wxChrome.h = winDefault(std::min(780.0f, inp.screenH - 160.0f), 0.74f, inp.screenH, 380.0f);
    }
    // Planetarium / Potato (knockout 32768): no volumetric clouds, so no storms, rain, fog or radar to show. The
    // window says so and runs nothing (no radar, no storm search).
    if (weatherUnavailable())
    {
        wxRadarWanted = false;
        static char offTitle[] = "Weather";
        buildResizableWindow(
            inp, ui, wxChrome, 12, offTitle, true, hovWxClose, inp.screenW - wxChrome.w - 40.0f, 90.0f, 360.0f, 160.0f,
            1400.0f, 1400.0f,
            [&]()
            {
                CLAY(CLAY_ID("WxOffBody"), {.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0)},
                                                       .padding = {14, 14, 12, 12},
                                                       .childGap = 8,
                                                       .layoutDirection = CLAY_TOP_TO_BOTTOM}})
                {
                    CLAY_TEXT(txc(Txt::NoteWeatherOffTitle),
                              CLAY_TEXT_CONFIG({.textColor = Pal::textBright, .fontSize = fs(13),
                                                .wrapMode = CLAY_TEXT_WRAP_WORDS}));
                    CLAY_TEXT(txc(Txt::NoteWeatherOffBody),
                              CLAY_TEXT_CONFIG({.textColor = Pal::textDim, .fontSize = fs(12),
                                                .wrapMode = CLAY_TEXT_WRAP_WORDS}));
                }
            });
        wxPendingAct = 0;
        wxFeatPendingSlot = -1;
        wxCycPendingAct = 0;
        return;
    }
    weatherRadarReadback();
    weatherRefreshRows(false);
    weatherRefreshCyclones(false);   // (every 2 s: the cyclone lists, for the Cyclones tab and the radar's tracks)
    weatherFeaturedUpdate();         // (the Storms tab's featured events: a bounded forward search a frame)
    const double now = (double)simDayJ2000 * 86400.0 + simSecInDay;
    auto text = [&](const char *s, Clay_Color c, int size, bool wrap = false) {
        CLAY_TEXT(cs(s), CLAY_TEXT_CONFIG({.textColor = c, .fontSize = fs(size),
                                           .wrapMode = wrap ? CLAY_TEXT_WRAP_WORDS : CLAY_TEXT_WRAP_NONE}));
    };
    static char titleBuf[32];
    snprintf(titleBuf, sizeof(titleBuf), "Weather");
    const float bodyW = std::max(200.0f, wxChrome.w - 24.0f - 8.0f - 10.0f);
    const float bodyH = std::max(200.0f, wxChrome.h - 40.0f);
    buildResizableWindow(
        inp, ui, wxChrome, 12, titleBuf, true, hovWxClose, inp.screenW - wxChrome.w - 40.0f, 90.0f, 360.0f, 320.0f, 1400.0f,
        1400.0f,
        [&]()
        {
            CLAY(CLAY_ID("WxBody"), {.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0)},
                                                .padding = {12, 12, 8, 10},
                                                .childGap = 6,
                                                .layoutDirection = CLAY_TOP_TO_BOTTOM}})
            {
                CLAY(CLAY_ID("WxTabs"), {.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0)},
                                                    .childGap = 4,
                                                    .childAlignment = {.y = CLAY_ALIGN_Y_CENTER},
                                                    .layoutDirection = CLAY_LEFT_TO_RIGHT}})
                {
                    // Drawn Storms, Radar, Cyclones, Controls (issue 20261028_222628: open on the events, like Missions and
                    // Sky events); wxTab keeps its values (0 Radar, 1 Storms, 2 Controls, 3 Cyclones; default 1).
                    static const char *kTabs[] = {tx(Txt::OptRadar), tx(Txt::OptStorms), tx(Txt::TtlControls), tx(Txt::OptCyclones)};
                    static const char *kTips[] = {tx(Txt::OptALocalPrecipitationRadarCentredOn),
                                                  tx(Txt::OptTheStormsNearYouAndThe),
                                                  tx(Txt::OptTheStormObjectsSettingsAlsoSettings),
                                                  tx(Txt::OptNamedStormsTheTropicalCyclonesTheir)};
                    static const int kOrder[] = {1, 0, 3, 2};
                    for (int i : kOrder)
                        if (uiButton(inp, ui, "WxTab", i, kTabs[i], kTips[i], wxTab == i))
                            wxTab = i;
                    CLAY(CLAY_ID("WxTabsGap"), {.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0)}}}) {}
                    text(wxStr(tx(Txt::FmtStormsAlive), wxStormsAlive), Pal::textDim, 11);
                }
                if (wxStatus != wxStatusShown)
                {
                    wxStatusShown = wxStatus;
                    wxStatusWall = glfwGetTime();
                }
                if (!wxStatus.empty() && glfwGetTime() - wxStatusWall > 12.0)
                    wxStatus.clear();
                if (!wxStatus.empty())
                    text(wxStr("%s", wxStatus.c_str()), Pal::textWarn, 11, true);
                CLAY(CLAY_ID("WxList"), {.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0)},
                                                    .padding = {0, 8, 2, 4},
                                                    .childGap = 5,
                                                    .layoutDirection = CLAY_TOP_TO_BOTTOM},
                                         .clip = {.vertical = true, .childOffset = Clay_GetScrollOffset()}})
                {
                    if (wxTab == 0)
                        buildWeatherRadarTab(inp, ui, bodyW, bodyH);
                    else if (wxTab == 1)
                        buildWeatherStormsTab(inp, ui);
                    else if (wxTab == 3)
                        buildWeatherCyclonesTab(inp, ui, bodyW);
                    else
                        buildWeatherControlsTab(inp, ui);
                }
                ui.scrollbar(CLAY_ID("WxList"));
            }
        });
    (void)now;
    // After the layout: the actions (Go to / Watch: they move time and the observer; the rows are rebuilt next frame).
    if (wxPendingAct != 0 && wxPendingId != 0)
    {
        std::string msg;
        if (wxPendingAct == 1)
            weatherGoToStorm(wxPendingId, msg);
        else if (wxPendingAct == 3)
            weatherWatchTornado(wxPendingId, msg);
        else if (wxPendingAct == 4)
            weatherWatchPassage(wxPendingId, msg);
        else if (wxPendingAct == 5)
            weatherWatchStormNow(wxPendingId, msg);
        else
            weatherWatchStorm(wxPendingId, msg);
        wxStatus = msg;
        wxRowsWall = -1e300;
    }
    wxPendingAct = 0;
    if (wxFeatPendingSlot >= 0)
    {
        std::string msg;
        weatherFeaturedAct(wxFeatPendingSlot, wxFeatPendingAct, msg);
        wxStatus = msg;
        wxRowsWall = wxFeatNowWall = -1e300;
        wxFeatPendingSlot = -1;
    }
    if (wxCycPendingAct != 0 && wxCycPendingId != 0)
    {
        std::string msg;
        if (wxCycPendingAct == 1)
            weatherGoToCyclone(wxCycPendingId, wxCycPendingArg, msg);
        else if (wxCycPendingAct == 2)
            weatherWatchCyclone(wxCycPendingId, msg);
        else
            weatherCycloneMilestone(wxCycPendingId, wxCycPendingArg, msg);
        wxStatus = msg;
        wxCycWall = wxRowsWall = -1e300;
    }
    wxCycPendingAct = 0;
    wxCycPendingId = 0;
    wxPendingId = 0;
}

void SatelliteSim::buildWeatherRadarTab(const UIInput &inp, UIRenderer &ui, float bodyW, float bodyH)
{
    const double now = (double)simDayJ2000 * 86400.0 + simSecInDay;
    auto text = [&](const char *s, Clay_Color c, int size, bool wrap = false) {
        CLAY_TEXT(cs(s), CLAY_TEXT_CONFIG({.textColor = c, .fontSize = fs(size),
                                           .wrapMode = wrap ? CLAY_TEXT_WRAP_WORDS : CLAY_TEXT_WRAP_NONE}));
    };
    // Ask for a fresh radar: at most 4 times a second, and only when the time, the place or the range changed.
    {
        const float cellM = 2000.0f * kWxRangesKm[std::clamp(wxRangeIdx, 0, 2)] / (float)kWxN;
        const glm::dvec3 o = glm::normalize(glm::dvec3(obsDir));
        const bool changed = !wxShown.valid || wxShown.cellM != cellM || std::abs(wxRadarDispSim - now) > 0.5 ||
                             std::acos(std::clamp(glm::dot(o, wxRadarDispObs), -1.0, 1.0)) * kRe > 0.25 * cellM;
        if (changed && !wxRadarInFlight && wallNow() - wxRadarDispWall >= 0.25)
            wxRadarWanted = true;
    }
    // The loop: through the frames taken while it was open, ~3 a second, holding on the latest.
    const WxRadarFrame *showF = &wxShown;
    if (wxLoop && wxLoopFrames.size() >= 2)
    {
        const double w = glfwGetTime();
        if (w - wxLoopWall > (wxLoopPos == (int)wxLoopFrames.size() - 1 ? 1.2 : 0.33))
        {
            wxLoopWall = w;
            wxLoopPos = (wxLoopPos + 1) % (int)wxLoopFrames.size();
            weatherRadarRaster(wxLoopFrames[(size_t)wxLoopPos]);
            wxLoopShownPos = wxLoopPos;
        }
        if (wxLoopShownPos >= 0 && wxLoopShownPos < (int)wxLoopFrames.size())
            showF = &wxLoopFrames[(size_t)wxLoopShownPos];
    }
    else if (wxLoopShownPos >= 0)
    {
        wxLoopShownPos = -1;
        wxLoopPos = -1;
        weatherRadarRaster(wxShown);
    }

    const bool haveImg = weatherRadarEnsureImage(ui);
    const float G = std::floor(std::clamp(std::min(bodyW - 8.0f, bodyH - (float)fs(11) * 17.0f), 160.0f, 640.0f));
    const float radiusKm = kWxRangesKm[std::clamp(wxRangeIdx, 0, 2)];

    // The markers: the storms' strongest towers on the shown frame's map (by the storm list's rank near you).
    wxMapStorms.clear();
    const bool looping = showF != &wxShown;   // the towers are now's: no markers on a past frame
    if (showF->valid && !stormTowers.empty() && !looping)
    {
        const glm::dvec3 up = showF->obs;
        const glm::dvec3 E = glm::normalize(glm::cross(glm::dvec3(0, 0, 1), up) + glm::dvec3(1e-12, 0, 0));
        const glm::dvec3 Nn = glm::cross(up, E);
        const double halfM = showF->cellM * kWxN * 0.5;
        for (const wx::Tower &tw : stormTowers)
        {
            if (tw.strength < 0.15f || tw.kind == wx::StormKind::Tropical)
                continue;   // (a cyclone's eyewall towers: the cyclone is drawn at its centre, below)
            bool dup = false;
            for (WxMapStorm &m : wxMapStorms)
                if (m.id == tw.stormId)
                {
                    dup = true; // (strongest first: the first tower listed is the one shown)
                    break;
                }
            if (dup)
                continue;
            const glm::dvec3 e = weatherMapToEarth(tw.dir, now);
            const double d = std::acos(std::clamp(glm::dot(e, up), -1.0, 1.0)) * kRe;
            if (d > halfM * 1.42)
                continue;
            const glm::dvec3 to = e - up * glm::dot(e, up);
            const double tl = glm::length(to);
            const double b = tl > 0.0 ? std::atan2(glm::dot(to, E), glm::dot(to, Nn)) : 0.0;
            const float x = (float)(0.5 + 0.5 * d * std::sin(b) / halfM), y = (float)(0.5 - 0.5 * d * std::cos(b) / halfM);
            if (x < 0.0f || x > 1.0f || y < 0.0f || y > 1.0f)
                continue;
            int rank = 0;
            for (size_t k = 0; k < wxNear.size(); ++k)
                if (wxNear[k].id == tw.stormId)
                    rank = (int)k + 1;
            wxMapStorms.push_back({x, y, tw.stormId, (int)tw.kind, rank, tw.strength});
        }
    }
    wxStats.stormsOnMap = (int)wxMapStorms.size();
    // Hover / click on a marker.
    const Clay_ElementId rid = CLAY_ID("WxRadarImg");
    const Clay_ElementData rd = Clay_GetElementData(rid);
    wxHoverStorm = -1;
    if (rd.found && rd.boundingBox.width > 1.0f)
    {
        const float mx = (inp.mouseX - rd.boundingBox.x) / rd.boundingBox.width;
        const float my = (inp.mouseY - rd.boundingBox.y) / rd.boundingBox.height;
        if (mx >= 0.0f && mx <= 1.0f && my >= 0.0f && my <= 1.0f)
        {
            float best = 10.0f / rd.boundingBox.width;
            for (size_t k = 0; k < wxMapStorms.size(); ++k)
            {
                const float d = std::hypot(wxMapStorms[k].x - mx, wxMapStorms[k].y - my);
                if (d < best)
                {
                    best = d;
                    wxHoverStorm = (int)k;
                }
            }
            if (inp.lmbPressed && wxHoverStorm >= 0)
                wxSelStorm = wxMapStorms[(size_t)wxHoverStorm].id;
        }
    }
    // The overlay: markers, labels, you, the ring labels, north.
    wxQuads.clear();
    wxRuns.clear();
    wxRunText.clear();
    {
        auto ring = [&](float x, float y, float r, float th, glm::vec4 col) {
            wxQuads.push_back({x - r, y - r, 2.0f * r, th, col});
            wxQuads.push_back({x - r, y + r - th, 2.0f * r, th, col});
            wxQuads.push_back({x - r, y - r + th, th, 2.0f * r - 2.0f * th, col});
            wxQuads.push_back({x + r - th, y - r + th, th, 2.0f * r - 2.0f * th, col});
        };
        auto label = [&](float x, float y, const char *s) {
            wxRunText.emplace_back(s);
            wxRuns.push_back({wxRunText.back().c_str(), (int)wxRunText.back().size(), x, y});
        };
        const float pulse = 0.5f + 0.5f * (float)std::sin(glfwGetTime() * 4.0);
        for (size_t k = 0; k < wxMapStorms.size(); ++k)
        {
            const WxMapStorm &m = wxMapStorms[k];
            const float x = m.x * G, y = m.y * G;
            const bool hot = (int)k == wxHoverStorm || m.id == wxSelStorm;
            const float r = 4.0f + 3.0f * std::clamp(m.strength, 0.0f, 1.0f) + (hot ? 1.5f + pulse : 0.0f);
            ring(x, y, r + 1.0f, 1.0f, glm::vec4(0.0f, 0.0f, 0.0f, 0.7f));
            ring(x, y, r, hot ? 2.0f : 1.5f, glm::vec4(1.0f, 1.0f, 1.0f, 0.95f));
            if (wxShowLabels)
            {
                char nb[24];
                if (m.rank > 0)
                    snprintf(nb, sizeof(nb), "%d %s", m.rank, kindShort(m.kind));
                else
                    snprintf(nb, sizeof(nb), "%s", kindShort(m.kind));
                label(x + r + 2.0f, y - r - (float)fs(9) * 0.5f, nb);
            }
        }
        // Tropical cyclones in range: the track (hourly dots in Saffir-Simpson colours, faint ahead) and the centre.
        if (showF->valid && !looping)
        {
            const glm::dvec3 up = showF->obs;
            const glm::dvec3 E = glm::normalize(glm::cross(glm::dvec3(0, 0, 1), up) + glm::dvec3(1e-12, 0, 0));
            const glm::dvec3 Nn = glm::cross(up, E);
            const double halfM = showF->cellM * kWxN * 0.5;
            auto proj = [&](const glm::dvec3 &e, float &x, float &y) -> bool {
                const double d = std::acos(std::clamp(glm::dot(e, up), -1.0, 1.0)) * kRe;
                if (d > halfM * 1.5) return false;
                const glm::dvec3 to = e - up * glm::dot(e, up);
                const double b = glm::length(to) > 0.0 ? std::atan2(glm::dot(to, E), glm::dot(to, Nn)) : 0.0;
                x = (float)(0.5 + 0.5 * d * std::sin(b) / halfM) * G;
                y = (float)(0.5 - 0.5 * d * std::cos(b) / halfM) * G;
                return x >= 0.0f && x <= G && y >= 0.0f && y <= G;
            };
            static const float kSS[7][3] = {{94, 186, 255}, {0, 250, 244}, {255, 255, 204}, {255, 231, 117},
                                            {255, 193, 64}, {255, 143, 32}, {255, 96, 96}};
            for (const wx::Cyclone &c : wxCycActive)
            {
                const size_t step = radiusKm <= 60.0f ? 1 : radiusKm <= 200.0f ? 1 : 3;
                for (size_t k = 0; k < c.track.size(); k += step)
                {
                    float x, y;
                    if (!proj(c.track[k], x, y)) continue;
                    const float v = c.vmax[k];
                    const int cat = v < 17.5f ? -1 : v < 33.0f ? 0 : v < 43.0f ? 1 : v < 50.0f ? 2 : v < 58.0f ? 3 : v < 70.0f ? 4 : 5;
                    const float *q = kSS[cat + 1];
                    const bool past = c.t0 + (double)k * wx::Cyclone::kStepS <= now;
                    const float r = past ? 1.5f : 1.0f;
                    wxQuads.push_back({x - r, y - r, 2.0f * r, 2.0f * r, glm::vec4(q[0] / 255.0f, q[1] / 255.0f, q[2] / 255.0f, past ? 1.0f : 0.55f)});
                }
                float x, y;
                if (now >= c.t0 && now <= c.t0 + c.life && proj(c.earthAt(now), x, y))
                {
                    ring(x, y, 7.0f, 2.0f, glm::vec4(1.0f, 1.0f, 1.0f, 0.95f));
                    label(x + 9.0f, y - (float)fs(9) * 0.5f, c.name.empty() ? "TD" : c.name.c_str());
                }
            }
        }
        // You: a cyan cross at the centre.
        const float c = 0.5f * G;
        const glm::vec4 you = palV(Pal::trkYou, 1.0f);
        wxQuads.push_back({c - 6.0f, c - 1.0f, 12.0f, 2.0f, you});
        wxQuads.push_back({c - 1.0f, c - 6.0f, 2.0f, 12.0f, you});
        // The rings' distances along the north line, and N.
        const float stepKm = radiusKm <= 60.0f ? 10.0f : radiusKm <= 200.0f ? 50.0f : 100.0f;
        for (float rk = stepKm; rk <= radiusKm + 0.1f; rk += stepKm)
        {
            if (radiusKm <= 60.0f && std::fmod(rk, 20.0f) > 0.1f && rk < radiusKm - 0.1f)
                continue;   // every other ring at 50 km
            char nb[16];
            snprintf(nb, sizeof(nb), "%.0f", rk);
            label(c + 3.0f, c - rk / radiusKm * c - (float)fs(9) - 1.0f, nb);
        }
        label(c - (float)fs(9) * 0.3f, 2.0f, "N");
    }
    wxMarks.runs = wxRuns.data();
    wxMarks.runCount = (int)wxRuns.size();
    wxMarks.quads = wxQuads.data();
    wxMarks.quadCount = (int)wxQuads.size();
    wxMarks.fontSize = (float)fs(9);
    wxMarks.color = glm::vec4(0.92f, 0.94f, 0.96f, 1.0f);
    wxImgEl.imageId = wxUiId;

    // The radar.
    CLAY(CLAY_ID("WxRadarRow"), {.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0)},
                                            .childAlignment = {.x = CLAY_ALIGN_X_CENTER},
                                            .layoutDirection = CLAY_LEFT_TO_RIGHT}})
    {
        CLAY(CLAY_ID("WxRadarBack"), {.layout = {.sizing = {CLAY_SIZING_FIXED(G), CLAY_SIZING_FIXED(G)}},
                                      .backgroundColor = Pal::insetBg,
                                      .cornerRadius = CLAY_CORNER_RADIUS(4)})
        {
            if (haveImg)
            {
                CLAY(rid, {.layout = {.sizing = {CLAY_SIZING_FIXED(G), CLAY_SIZING_FIXED(G)}}, .custom = {.customData = &wxImgEl}})
                {
                    CLAY(CLAY_ID("WxRadarMarks"), {.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0)}},
                                                   .custom = {.customData = &wxMarks}}) {}
                }
            }
        }
    }
    if (wxHoverStorm >= 0 && wxHoverStorm < (int)wxMapStorms.size())
    {
        WxStormRow r;
        if (weatherStormRowCached(wxMapStorms[(size_t)wxHoverStorm].id, r))
        {
            char a[32], l[32];
            fmtDur(now - r.t0, a, sizeof(a));
            fmtDur(r.tEnd - now, l, sizeof(l));
            ui.tooltip(inp, true, wxStr(tx(Txt::FmtOldLeftFlashesMin), kindName(r.kind), stageName(r.stage), a, l,
                                        r.flashesPerMin), fs(11));
        }
    }
    // The time of the frame shown, and the scale.
    {
        char u[48];
        const time_t unixSim = (time_t)std::floor(showF->valid ? showF->simT : now) + 946728000;
        const struct tm *g = gmtime(&unixSim);
        if (g)
            snprintf(u, sizeof(u), tx(Txt::FmtUtc4), g->tm_hour, g->tm_min);
        else
            snprintf(u, sizeof(u), "--");
        const double ago = showF->valid ? now - showF->simT : 0.0;
        if (!wxRadarRanCapable && !wxShown.valid)
            text(tx(Txt::NoteTheRadarNeedsTheCloudRenderer), Pal::textWarn, 11, true);
        else if (wxLoop && wxLoopFrames.size() >= 2)
            text(wxStr(tx(Txt::FmtLoopOfFramesMinAgo), u, wxLoopShownPos + 1, wxLoopFrames.size(), ago / 60.0),
                 Pal::volValue, 11);
        else
            text(wxStr(tx(Txt::FmtRadarKmAroundYouTheRain), u, radiusKm, kWxSampleAglM),
                 Pal::textDim, 11, true);
    }
    // The colour scale.
    CLAY(CLAY_ID("WxScale"), {.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0)},
                                         .childGap = 1,
                                         .childAlignment = {.y = CLAY_ALIGN_Y_CENTER},
                                         .layoutDirection = CLAY_LEFT_TO_RIGHT}})
    {
        text("dBZ", Pal::textDim, 10);
        CLAY(CLAY_ID("WxScaleGap0"), {.layout = {.sizing = {CLAY_SIZING_FIXED(4), CLAY_SIZING_FIT(0)}}}) {}
        const float sw = std::clamp((G - 60.0f) / 13.0f, 10.0f, 26.0f);
        int k = 0;
        for (float dbz = 10.0f; dbz <= 70.0f; dbz += 5.0f, ++k)
        {
            const glm::vec3 c = ramp(kRain, dbz);
            CLAY(CLAY_IDI("WxScaleSw", (uint32_t)k), {.layout = {.sizing = {CLAY_SIZING_FIXED(sw), CLAY_SIZING_FIXED((float)fs(12))},
                                                                 .childAlignment = {.x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER}},
                                                      .backgroundColor = {c.r, c.g, c.b, 255}})
            {
                if (k % 2 == 0)
                    text(wxStr("%.0f", dbz), dbz >= 30.0f && dbz < 45.0f ? Pal::panelSolid : Pal::textBright, 9);
            }
        }
    }
    CLAY(CLAY_ID("WxScale2"), {.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0)},
                                          .childGap = 4,
                                          .childAlignment = {.y = CLAY_ALIGN_Y_CENTER},
                                          .layoutDirection = CLAY_LEFT_TO_RIGHT}})
    {
        auto sw = [&](uint32_t id, glm::vec3 c, const char *s) {
            CLAY(CLAY_IDI("WxSw2", id), {.layout = {.sizing = {CLAY_SIZING_FIXED(10), CLAY_SIZING_FIXED(10)}},
                                         .backgroundColor = {c.r, c.g, c.b, 255},
                                         .cornerRadius = CLAY_CORNER_RADIUS(2)}) {}
            text(s, Pal::textDim, 10);
        };
        sw(0, ramp(kRain, 30.0f), "rain");
        sw(1, ramp(kMix, 25.0f), "mixed");
        sw(2, ramp(kSnow, 25.0f), "snow");
        sw(3, glm::vec3(255.0f), "storms");
        const Clay_Color yc = Pal::trkYou;
        sw(4, glm::vec3(yc.r, yc.g, yc.b), "you");
    }
    // Range and the toggles.
    {
        static const char *kR[] = {tx(Txt::OptKm3), tx(Txt::OptKm2), tx(Txt::OptKm)};
        const int p = uiChoiceRow(inp, ui, "WxRange", 0, tx(Txt::OptRange), kR, 3, wxRangeIdx, tx(Txt::TipTheRadarsRadius));
        if (p >= 0)
            wxRangeIdx = p;
    }
    CLAY(CLAY_ID("WxToggles"), {.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0)},
                                           .childGap = 4,
                                           .childAlignment = {.y = CLAY_ALIGN_Y_CENTER},
                                           .layoutDirection = CLAY_LEFT_TO_RIGHT}})
    {
        if (uiButton(inp, ui, "WxClouds", 0, tx(Txt::BtnClouds), tx(Txt::TipTheLowCloudsCoverFaintlyUnder), wxShowClouds))
        {
            wxShowClouds = !wxShowClouds;
            weatherRadarRaster(wxShown);
        }
        if (uiButton(inp, ui, "WxLabels", 0, tx(Txt::BtnLabels), tx(Txt::TipTheStormsLabelsTheNumberIs),
                     wxShowLabels))
            wxShowLabels = !wxShowLabels;
        if (uiButton(inp, ui, "WxLoop", 0, tx(Txt::BtnLoop),
                     tx(Txt::TipPlayTheFramesTakenWhileThe),
                     wxLoop, wxLoopFrames.size() >= 2 || wxLoop))
            wxLoop = !wxLoop;
        CLAY(CLAY_ID("WxTogGap"), {.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0)}}}) {}
        if (wxStats.maxDbz > 0.0f)
            text(wxStr(tx(Txt::FmtMaxDbz), wxStats.maxDbz), Pal::textDim, 10);
    }
    // The selected storm (a click on its marker).
    WxStormRow sel;
    if (wxSelStorm != 0 && weatherStormRowCached(wxSelStorm, sel))
    {
        char a[32], l[32];
        fmtDur(now - sel.t0, a, sizeof(a));
        fmtDur(sel.tEnd - now, l, sizeof(l));
        CLAY(CLAY_ID("WxSel"), {.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0)},
                                           .padding = {8, 8, 5, 5},
                                           .childGap = 8,
                                           .childAlignment = {.y = CLAY_ALIGN_Y_CENTER},
                                           .layoutDirection = CLAY_LEFT_TO_RIGHT},
                                .backgroundColor = Pal::rowHighlight,
                                .cornerRadius = CLAY_CORNER_RADIUS(4)})
        {
            CLAY(CLAY_ID("WxSelT"), {.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0)},
                                                .childGap = 2,
                                                .layoutDirection = CLAY_TOP_TO_BOTTOM}})
            {
                text(wxStr("%s, %s", kindName(sel.kind), stageName(sel.stage)), Pal::textPrimary, 12, true);
                text(wxStr(tx(Txt::FmtKmOldLeftFlashesMin), sel.distKm, compass(sel.bearingDeg), a, l,
                           sel.flashesPerMin),
                     Pal::textDim, 11, true);
            }
            if (uiButton(inp, ui, "WxSelWatch", 0, tx(Txt::BtnWatch), tx(Txt::TipTheStormAtItsHeightFrom)))
            {
                wxPendingAct = 5;
                wxPendingId = sel.id;
            }
        }
    }
    else
        text(tx(Txt::NoteClickAStormMarkerForIts),
             Pal::textHint, 10, true);
}

// A storm's row from this refresh's lists, else computed now.
bool SatelliteSim::weatherStormRowCached(uint64_t id, WxStormRow &out)
{
    for (const WxStormRow &r : wxNear)
        if (r.id == id)
        {
            out = r;
            return true;
        }
    for (const WxStormRow &r : wxWorld)
        if (r.id == id)
        {
            out = r;
            return true;
        }
    return weatherStormNow(id, (double)simDayJ2000 * 86400.0 + simSecInDay, out);
}

void SatelliteSim::buildWeatherStormsTab(const UIInput &inp, UIRenderer &ui)
{
    const double now = (double)simDayJ2000 * 86400.0 + simSecInDay;
    auto text = [&](const char *s, Clay_Color c, int size, bool wrap = false) {
        CLAY_TEXT(cs(s), CLAY_TEXT_CONFIG({.textColor = c, .fontSize = fs(size),
                                           .wrapMode = wrap ? CLAY_TEXT_WRAP_WORDS : CLAY_TEXT_WRAP_NONE}));
    };
    auto tag = [&](uint32_t id, const char *label, Clay_Color bg) {
        CLAY(CLAY_IDI("WxTag", id), {.layout = {.sizing = {CLAY_SIZING_FIT(0), CLAY_SIZING_FIT(0)}, .padding = {5, 5, 1, 1}},
                                     .backgroundColor = bg,
                                     .cornerRadius = CLAY_CORNER_RADIUS(3)})
        {
            text(label, Pal::textBright, 10);
        }
    };
    auto stageCol = [](int s) -> Clay_Color {
        switch (s)
        {
        case 1: return Pal::btnDanger;      // (orange)
        case 2: return Pal::progressFill;   // (red)
        case 3: return Pal::tagTwilight;    // (purple)
        default: return Pal::trackBg;
        }
    };
    auto row = [&](int id, const WxStormRow &r, int rank) {
        // A fixed height (four single lines, a 2 x 2 button grid), so the tab never changes size as storms come and go.
        char a[32], l[32];
        fmtDur(now - r.t0, a, sizeof(a));
        fmtDur(r.tEnd - now, l, sizeof(l));
        const bool sel = r.id == wxSelStorm;
        const float lineH = (float)fs(12) * 1.35f;
        static const int kIcons[] = {30, 30, 26, 27, 27, 24};   // (SatelliteSimWeatherFeatured.cpp's storm icons by kind)
        const int icon = kIcons[std::clamp(r.kind, 0, 5)];
        CLAY(CLAY_IDI("WxSRow", (uint32_t)id), {.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(4.0f * lineH + 16.0f)},
                                                           .padding = {8, 8, 6, 6},
                                                           .childGap = 8,
                                                           .childAlignment = {.y = CLAY_ALIGN_Y_CENTER},
                                                           .layoutDirection = CLAY_LEFT_TO_RIGHT},
                                                .backgroundColor = sel ? Pal::rowHighlight : Pal::cardBg,
                                                .cornerRadius = CLAY_CORNER_RADIUS(4),
                                                .border = {.color = Pal::cardBorder, .width = CLAY_BORDER_ALL(1)}})
        {
            CLAY(CLAY_IDI("WxSIcon", (uint32_t)id), {.layout = {.sizing = {CLAY_SIZING_FIXED(26), CLAY_SIZING_FIXED(26)}},
                                                     .image = {.imageData = (void *)(intptr_t)(icon + 1)}}) {}
            CLAY(CLAY_IDI("WxSText", (uint32_t)id), {.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0)},
                                                                .childGap = 2,
                                                                .childAlignment = {.y = CLAY_ALIGN_Y_CENTER},
                                                                .layoutDirection = CLAY_TOP_TO_BOTTOM},
                                                     .clip = {.horizontal = true}})
            {
                CLAY(CLAY_IDI("WxSTitle", (uint32_t)id), {.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0)},
                                                                     .childGap = 6,
                                                                     .childAlignment = {.y = CLAY_ALIGN_Y_CENTER},
                                                                     .layoutDirection = CLAY_LEFT_TO_RIGHT}})
                {
                    tag((uint32_t)id, wxStr("%s", stageName(r.stage)), stageCol(r.stage));
                    if (rank > 0)
                        text(wxStr("%d. %s", rank, kindName(r.kind)), Pal::textPrimary, 12);
                    else
                        text(wxStr("%s", kindName(r.kind)), Pal::textPrimary, 12);
                    if (r.towerCount > 1)
                        text(wxStr(tx(Txt::FmtOfTowers), r.towersAlive, r.towerCount), Pal::textDim, 11);
                }
                const glm::dvec3 &e = r.e;
                const double lat = glm::degrees(std::asin(std::clamp(e.z, -1.0, 1.0))), lon = glm::degrees(std::atan2(e.y, e.x));
                text(wxStr(tx(Txt::FmtKmDeg), r.distKm, compass(r.bearingDeg), r.bearingDeg, std::abs(lat),
                           lat >= 0.0 ? "N" : "S", std::abs(lon), lon >= 0.0 ? "E" : "W"),
                     Pal::textDim, 11);
                text(wxStr(tx(Txt::FmtOldLeftStrengthFlashesMin), a, l, 100.0f * r.strength, r.flashesPerMin),
                     Pal::textDim, 11);
                if (r.tTorn0 > 0.0)
                {
                    char w[32];
                    const bool on = now >= r.tTorn0 && now < r.tTorn1;
                    fmtDur(std::abs((on ? r.tTorn1 : r.tTorn0) - now), w, sizeof(w));
                    text(on ? wxStr(tx(Txt::FmtTornadoOnTheGroundNowMore), w)
                            : r.tTorn0 > now ? wxStr(tx(Txt::FmtTornadoTouchesDownInForMin), w, (r.tTorn1 - r.tTorn0) / 60.0)
                                             : wxStr(tx(Txt::FmtTornadoTouchedDownAgo), w),
                         on ? Pal::textBright : Pal::textPrimary, 11);
                }
                else if (r.tPass > 0.0)
                {
                    char w[32];
                    fmtDur(r.tPass - now, w, sizeof(w));
                    text(wxStr(tx(Txt::FmtItsLineReachesYouIn), w), Pal::textPrimary, 11);
                }
                else
                    text(" ", Pal::textDim, 11);
            }
            CLAY(CLAY_IDI("WxSBtns", (uint32_t)id), {.layout = {.sizing = {CLAY_SIZING_FIXED((float)fs(11) * 6.0f), CLAY_SIZING_FIT(0)},
                                                                .childAlignment = {.y = CLAY_ALIGN_Y_CENTER},
                                                                .layoutDirection = CLAY_TOP_TO_BOTTOM}})
            {
                const bool tornNow = r.tTorn0 > 0.0 && now >= r.tTorn0 + 30.0 && now <= r.tTorn1 - 180.0;
                if (uiButton(inp, ui, "WxSWatch", id, tx(Txt::BtnWatch),
                             tornNow ? tx(Txt::TipItsTornadoOnTheGroundA) : tx(Txt::TipTheStormAtItsHeightFrom), false, true, true))
                {
                    wxPendingAct = 5;
                    wxPendingId = r.id;
                    wxSelStorm = r.id;
                }
            }
        }
    };
    if (!stormObjects)
    {
        text(tx(Txt::NoteStormObjectsAreOffControlsOr), Pal::textDim, 11, true);
        return;
    }
    // (2026-10-23, the user: reserved spots, the cool events first, storms near you after them, nothing moving about.)
    uiSection("WxSec", 2, tx(Txt::SecEvents));
    buildWeatherFeatured(inp, ui);
    uiSection("WxSec", 0, wxStr(tx(Txt::FmtNearYouWithinKm), kWxNearKm));
    for (int i = 0; i < kWxNearSlots; ++i)
    {
        const WxStormRow &r = i < (int)wxNearSlots.size() ? wxNearSlots[(size_t)i] : WxStormRow{};
        if (r.id != 0)
            row(i, r, 0);
        else
            CLAY(CLAY_IDI("WxSEmpty", (uint32_t)i), {.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(28)},
                                                              .padding = {8, 8, 6, 6},
                                                              .childAlignment = {.y = CLAY_ALIGN_Y_CENTER}},
                                                   .backgroundColor = Pal::cardBg,
                                                   .cornerRadius = CLAY_CORNER_RADIUS(4)})
            {
                text(i == 0 ? tx(Txt::NoteNoOrganisedStormNearYou) : "-", Pal::textHint, 11);
            }
    }
    text(tx(Txt::NoteStormsLiveInTheCloudMaps),
         Pal::textHint, 10, true);
}

void SatelliteSim::buildWeatherControlsTab(const UIInput &inp, UIRenderer &ui)
{
    auto text = [&](const char *s, Clay_Color c, int size, bool wrap = false) {
        CLAY_TEXT(cs(s), CLAY_TEXT_CONFIG({.textColor = c, .fontSize = fs(size),
                                           .wrapMode = wrap ? CLAY_TEXT_WRAP_WORDS : CLAY_TEXT_WRAP_NONE}));
    };
    uiSection("WxCSec", 0, tx(Txt::SecStormObjects));
    {
        bool v = stormObjects;
        if (uiToggleRow(inp, ui, "WxCOn", 0, tx(Txt::LblStormObjects), v, tx(Txt::TipOffNoStormTowersAtAll)))
        {
            stormObjects = v;
            graphicsPreset = GraphicsPreset::Custom;
            wxRowsWall = -1e300;
        }
    }
    // A value row: label, the value (a text box on click), - / +.
    auto num = [&](int id, const char *label, float &v, float vmin, float vmax, float step, const char *fmt, const char *tip) {
        CLAY(CLAY_IDI("WxCNum", (uint32_t)id), {.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIT((float)fs(20) + 6.0f)},
                                                           .padding = {2, 2, 2, 2},
                                                           .childGap = 6,
                                                           .childAlignment = {.y = CLAY_ALIGN_Y_CENTER},
                                                           .layoutDirection = CLAY_LEFT_TO_RIGHT}})
        {
            CLAY(CLAY_IDI("WxCNumL", (uint32_t)id), {.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0)}}})
            {
                const bool h = Clay_Hovered();
                ui.tooltip(inp, h, tip, fs(11));
                text(label, Pal::volLabel, 12);
            }
            const float old = v;
            numberField(inp, ui, CLAY_IDI("WxCNumV", (uint32_t)id), v, vmin, vmax, fmt, 58.0f, 12, Pal::volValue);
            if (uiButton(inp, ui, "WxCMinus", id, "-", nullptr))
                v = sliderStep(v, -step, vmin, vmax);
            if (uiButton(inp, ui, "WxCPlus", id, "+", nullptr))
                v = sliderStep(v, step, vmin, vmax);
            if (v != old)
            {
                graphicsPreset = GraphicsPreset::Custom;
                wxRowsWall = -1e300;
            }
        }
    };
    num(0, tx(Txt::SldStormFrequencyX), stormFrequency, 0.0f, 10.0f, 0.05f, "%.2f", tx(Txt::TipXTheCalibratedSpawnRateClouds));
    num(1, tx(Txt::SldStormLifetimeX), stormLifeScale, 0.1f, 5.0f, 0.05f, "%.2f", tx(Txt::TipXEveryStormsLifetimeCloudsV2));
    num(2, tx(Txt::SldStormCloudField), stormCloudField, 0.0f, 3.0f, 0.05f, "%.2f",
        tx(Txt::TipEachStormsOwnCloudInThe));
    num(3, tx(Txt::SldStormClearMoat), stormMoat, 0.0f, 3.0f, 0.05f, "%.2f", tx(Txt::TipTheClearMoatRoundAStorm));
    num(6, tx(Txt::SldHurricaneArmCirrus), stormArmCirrus, 0.0f, 3.0f, 0.05f, "%.2f",
        tx(Txt::TipTheIceStreamingAlongATropical));
    num(10, tx(Txt::SldStormSurgeX), stormSurge, 0.0f, 2.0f, 0.05f, "%.2f",
        tx(Txt::TipXATropicalCyclonesStormSurge));
    uiSection("WxCSec", 2, tx(Txt::SecWildfires));
    num(7, tx(Txt::SldWildfiresX), wildfireRate, 0.0f, 10.0f, 0.05f, "%.2f",
        tx(Txt::TipXTheCalibratedFireRateFuel));
    num(8, tx(Txt::SldWildfireSmoke), wildfireSmoke, 0.0f, 5.0f, 0.05f, "%.2f",
        tx(Txt::TipXTheSmokesOpticalDepthThe));
    num(12, tx(Txt::SldSmokeBrightness), wildfireSmokeBright, 0.0f, 5.0f, 0.05f, "%.2f",
        tx(Txt::TipXTheLightTheSmokeScatters));
    num(13, tx(Txt::SldSmokeTint1Blue1Brown), wildfireSmokeTint, -1.0f, 1.0f, 0.05f, "%.2f",
        tx(Txt::TipTheSmokesOwnColour0Neutral));
    num(14, tx(Txt::SldSmokeSpreadX), wildfireSmokeSpread, 0.25f, 4.0f, 0.05f, "%.2f",
        tx(Txt::TipXThePlumesAndThePalls));
    num(15, tx(Txt::SldSmokeSpectralX), wildfireSmokeSpectral, 0.0f, 2.0f, 0.05f, "%.2f",
        tx(Txt::TipXTheSmokesAngstromExponent1));
    num(9, tx(Txt::SldWildfireLight), wildfireLight, 0.0f, 5.0f, 0.05f, "%.2f",
        tx(Txt::TipXTheFireFrontsLightThe));
    num(11, tx(Txt::SldHaboobsX), haboobGain, 0.0f, 5.0f, 0.05f, "%.2f",
        tx(Txt::TipXTheDustWallsStormsGust));
    {
        bool v = haboobField;
        if (uiToggleRow(inp, ui, "WxCOn", 1, tx(Txt::LblHaboobsAsCloud), v,
                        tx(Txt::TipOnTheDustWallsAreDrawn2)))
            haboobField = v;
    }
    uiSection("WxCSec", 1, tx(Txt::SecLightning));
    num(4, tx(Txt::SldLightningRateCbTowersMinEach), cv2LightningRate, 0.0f, 30.0f, 0.1f, "%.1f",
        tx(Txt::TipFlashesAMinuteOfAFull));
    num(5, tx(Txt::SldLightningRateStormsMinPer20), cv2LightningStormRate, 0.0f, 10.0f, 0.05f, "%.2f",
        tx(Txt::TipFlashesOverTheWeatherMapsDeep));
    if (uiButton(inp, ui, "WxCAll", 0, tx(Txt::BtnAllWeatherSettings), tx(Txt::TipSettingsWeatherRainLightningFogThe)))
    {
        settingsChrome.open = true;
        settingsActiveTab = 12;
        if (settingsTabIsAdvanced(12))
            showAdvancedSettings = true;
    }
    text(tx(Txt::NoteFrequencyAndLifetimeRebuildTheStorm),
         Pal::textHint, 10, true);
}

// ── Harness ─────────────────────────────────────────────────────────────────────────────────────────────────────
nlohmann::json SatelliteSim::weatherWindowJson()
{
    weatherRefreshRows(true);
    nlohmann::json j;
    j["open"] = wxChrome.open;
    j["tab"] = wxTab;
    j["range_km"] = kWxRangesKm[std::clamp(wxRangeIdx, 0, 2)];
    j["storms_alive"] = wxStormsAlive;
    auto rowJ = [&](const WxStormRow &r) {
        const double now = (double)simDayJ2000 * 86400.0 + simSecInDay;
        return nlohmann::json{{"id", r.id}, {"kind", kindName(r.kind)}, {"kind_id", r.kind}, {"stage", stageName(r.stage)},
                              {"age_min", (now - r.t0) / 60.0}, {"left_min", (r.tEnd - now) / 60.0},
                              {"mature_in_min", (r.tMature - now) / 60.0}, {"dist_km", r.distKm}, {"bearing_deg", r.bearingDeg},
                              {"lat_deg", glm::degrees(std::asin(std::clamp(r.e.z, -1.0, 1.0)))},
                              {"lon_deg", glm::degrees(std::atan2(r.e.y, r.e.x))}, {"strength", r.strength},
                              {"towers_alive", r.towersAlive}, {"towers", r.towerCount}, {"flashes_per_min", r.flashesPerMin},
                              {"tornado_in_min", r.tTorn0 > 0.0 ? nlohmann::json((r.tTorn0 - now) / 60.0) : nlohmann::json()},
                              {"tornado_min", r.tTorn0 > 0.0 ? nlohmann::json((r.tTorn1 - r.tTorn0) / 60.0) : nlohmann::json()},
                              {"passage_in_min", r.tPass > 0.0 ? nlohmann::json((r.tPass - now) / 60.0) : nlohmann::json()}};
    };
    nlohmann::json n = nlohmann::json::array(), w = nlohmann::json::array();
    for (const WxStormRow &r : wxNear)
        n.push_back(rowJ(r));
    for (const WxStormRow &r : wxWorld)
        w.push_back(rowJ(r));
    j["near"] = n;
    j["world"] = w;
    nlohmann::json ns = nlohmann::json::array();
    for (const WxStormRow &r : wxNearSlots)
        ns.push_back(r.id != 0 ? rowJ(r) : nlohmann::json());
    j["near_slots"] = ns;
    j["events"] = weatherFeaturedJson();
    nlohmann::json rd;
    rd["valid"] = wxShown.valid;
    rd["dispatches"] = wxRadarDispatches;
    rd["gpu_ms"] = wxRadarGpuMs;
    rd["cpu_ms"] = wxRadarCpuMs;   // the read back + the raster, on the frame after a dispatch
    rd["pipeline"] = wxRadarPipe != VK_NULL_HANDLE;
    rd["ui_image_id"] = wxUiId;
    rd["cell_m"] = wxShown.cellM;
    rd["frame_age_s"] = wxShown.valid ? (double)simDayJ2000 * 86400.0 + simSecInDay - wxShown.simT : -1.0;
    rd["max_rain"] = wxStats.maxRain;
    rd["max_dbz"] = wxStats.maxDbz;
    rd["raining_cells"] = wxStats.raining;
    rd["rain_share"] = wxStats.rainShare;
    rd["core_share"] = wxStats.coreShare;
    rd["snow_cells"] = wxStats.snowing;
    rd["centre_rain"] = wxStats.centreRain;
    rd["storms_on_map"] = wxStats.stormsOnMap;
    rd["loop_frames"] = wxLoopFrames.size();
    if (cv2FlashMapped)
    {
        const float *map = (const float *)((const char *)cv2FlashMapped + kCv2RainMapOffset);
        const int nn = 32, c = 16;
        rd["rain_amount"] = cv2RainAmount;   // the radar's unit x this = the extinction / 0.0012 the rain map holds (clamped 0..1)
        rd["eye_rain_map"] = 0.25f * (map[(c - 1) * nn + c - 1] + map[(c - 1) * nn + c] + map[c * nn + c - 1] + map[c * nn + c]);
        float mx = 0.0f;
        for (int q = 0; q < nn * nn; ++q)
            mx = std::max(mx, map[q]);
        rd["eye_rain_map_max"] = mx;   // +-640 m round the eye
    }
    // The rainiest cells (lat / lon / value), for a script to visit and compare with the rain at the eye there.
    if (wxShown.valid)
    {
        struct C { float v; int i, j; };
        std::vector<C> cs;
        for (int jj = 0; jj < kWxN; jj += 2)
            for (int ii = 0; ii < kWxN; ii += 2)
                cs.push_back({wxShown.rain[(size_t)jj * kWxN + ii], ii, jj});
        std::sort(cs.begin(), cs.end(), [](const C &a, const C &b) { return a.v > b.v; });
        const glm::dvec3 up = wxShown.obs;
        const glm::dvec3 E = glm::normalize(glm::cross(glm::dvec3(0, 0, 1), up) + glm::dvec3(1e-12, 0, 0));
        const glm::dvec3 Nn = glm::cross(up, E);
        nlohmann::json top = nlohmann::json::array();
        int taken = 0;
        for (const C &c : cs)
        {
            if (taken >= 8)
                break;
            const double x = ((c.i + 0.5) - 0.5 * kWxN) * wxShown.cellM, y = (0.5 * kWxN - (c.j + 0.5)) * wxShown.cellM;
            const double d = std::sqrt(x * x + y * y), a = d / kRe;
            const glm::dvec3 u = d > 0.0 ? (E * x + Nn * y) / d : E;
            const glm::dvec3 p = up * std::cos(a) + u * std::sin(a);
            bool near = false;
            for (const auto &t : top)
                near |= std::hypot(t["i"].get<int>() - c.i, t["j"].get<int>() - c.j) < 12.0;
            if (near)
                continue;
            top.push_back({{"i", c.i}, {"j", c.j}, {"rain", c.v}, {"dbz", c.v > 0.0f ? rainDbz(c.v) : 0.0f},
                           {"core", wxShown.core[(size_t)c.j * kWxN + c.i]},
                           {"lat_deg", glm::degrees(std::asin(std::clamp(p.z, -1.0, 1.0)))},
                           {"lon_deg", glm::degrees(std::atan2(p.y, p.x))}});
            ++taken;
        }
        rd["top_cells"] = top;
    }
    j["radar"] = rd;
    return j;
}
